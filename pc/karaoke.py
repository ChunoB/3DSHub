"""Karaokê: a letra da música que está tocando no PC rola no 3DS, e a sua voz ganha nota.

- A música é qualquer uma tocando no PC (Spotify, YouTube no navegador...): o nome, o
  artista e em que ponto ela está vêm do Windows (os mesmos controles de mídia da
  Segunda Tela).
- A letra sincronizada vem do lrclib.net (grátis, sem cadastro), uma vez por música.
- A nota: o 3DS descobre a nota que você está cantando (microfone) e manda só ela. O PC
  ouve o som que está tocando (loopback), vê quais notas a música está tocando agora
  (cromagrama) e compara. Oitava não importa: cantar uma oitava abaixo vale igual.

Protocolo (UDP 5016):
    3DS -> "3DSK" + u16 frequência (décimos de Hz; 0 = sem voz) + u8 volume (0-100)
    PC  -> "3DSK" + u8 tocando + u32 posição (ms) + u32 id da música + u8 nota da
           música (0 = Dó ... 11 = Si; 255 = nenhuma) + u8 acerto agora (0-100)
           + u32 pontos + u8 estado da letra (0 buscando, 1 achou, 2 sem letra)
           + texto "título\\nartista"
    3DS -> "3DSL" + u32 id + u16 parte          pede um pedaço da letra
    PC  -> "3DSL" + u32 id + u16 parte + u16 total + texto ("ms\\tlinha\\n"...)
"""

import asyncio
import json
import math
import re
import socket
import struct
import threading
import time
import urllib.parse
import urllib.request
import warnings
import zlib

try:
    import numpy as np
    import soundcard
except Exception:                         # sem som: a letra funciona, a nota não
    soundcard = None

from tela import texto

warnings.filterwarnings("ignore", message="data discontinuity")

PORTA = 5016
TAXA = 22050
PEDACO = 900                              # bytes de letra por pacote


def limpar_titulo(titulo, artista):
    """Títulos do YouTube vêm como "Artista - Música (Official Video)"."""
    t = re.sub(r"[\(\[][^\)\]]*(official|video|lyric|audio|clipe|ao vivo|live|hd|4k)[^\)\]]*[\)\]]", "",
               titulo, flags=re.I).strip()
    if " - " in t and (not artista or artista.lower() in t.lower() or "topic" in artista.lower()):
        a, t = t.split(" - ", 1)
        artista = a.strip()
    artista = re.sub(r"\s*-\s*topic$", "", artista or "", flags=re.I)
    return t.strip(), artista.strip()


def buscar_letra(titulo, artista, duracao_s):
    """[(ms, linha)] da letra sincronizada, ou [] se não achar."""
    t, a = limpar_titulo(titulo, artista)
    tentativas = [{"track_name": t, "artist_name": a}, {"q": f"{a} {t}".strip()}, {"q": t}]
    for params in tentativas:
        try:
            url = "https://lrclib.net/api/search?" + urllib.parse.urlencode(params)
            req = urllib.request.Request(url, headers={"User-Agent": "3DSHub (github.com/ChunoB/3DSHub)"})
            with urllib.request.urlopen(req, timeout=8) as r:
                resultados = json.load(r)
        except Exception as e:
            print("Karaokê: erro ao buscar a letra", repr(e))
            continue
        com_letra = [x for x in resultados if x.get("syncedLyrics")]
        if duracao_s:                     # a versão com a duração mais parecida
            com_letra.sort(key=lambda x: abs((x.get("duration") or 0) - duracao_s))
        if com_letra:
            linhas = []
            for linha in com_letra[0]["syncedLyrics"].splitlines():
                for m, s, resto in re.findall(r"\[(\d+):(\d+(?:\.\d+)?)\](.*)", linha):
                    linhas.append((int((int(m) * 60 + float(s)) * 1000), texto(resto.strip(), 60)))
            linhas.sort()
            if linhas:
                return linhas
    return []


class MidiaComTempo:
    """Música tocando no Windows, com a posição (o Windows avisa a posição de tempos
    em tempos; entre um aviso e outro, soma o tempo que passou)."""

    def __init__(self):
        self.loop = asyncio.new_event_loop()
        self.gerenciador = None
        from winrt.windows.media.control import GlobalSystemMediaTransportControlsSessionManager as G
        self.G = G

    async def _ler(self):
        if self.gerenciador is None:
            self.gerenciador = await self.G.request_async()
        s = self.gerenciador.get_current_session()
        if not s:
            return None
        p = await s.try_get_media_properties_async()
        t = s.get_timeline_properties()
        tocando = s.get_playback_info().playback_status == 4
        pos = t.position.total_seconds()
        if tocando:
            atualizado = t.last_updated_time
            agora = time.time()
            try:
                pos += max(0.0, agora - atualizado.timestamp())
            except Exception:
                pass
        return p.title or "", p.artist or "", tocando, pos, t.end_time.total_seconds()

    def ler(self):
        try:
            return self.loop.run_until_complete(self._ler())
        except Exception:
            self.gerenciador = None
            return None


class Ouvido:
    """Escuta o som do PC e guarda as 12 notas (Dó a Si) que mais soam agora."""

    def __init__(self):
        self.croma = None
        self.ativo_ate = 0.0
        if soundcard:
            threading.Thread(target=self._loop, daemon=True, name="karaoke-ouvido").start()

    def _loop(self):
        import ctypes
        ctypes.windll.ole32.CoInitializeEx(None, 0)          # o som do Windows usa COM
        freqs = np.fft.rfftfreq(4096, 1 / TAXA)
        faixa = (freqs > 80) & (freqs < 1000)
        notas = (np.round(12 * np.log2(freqs[faixa] / 440) + 69).astype(int)) % 12
        janela = np.hanning(4096)
        while True:
            if time.monotonic() > self.ativo_ate:
                self.croma = None
                time.sleep(0.3)
                continue
            try:
                alto = soundcard.default_speaker()
                entrada = soundcard.get_microphone(id=str(alto.name), include_loopback=True)
                with entrada.recorder(samplerate=TAXA, channels=1, blocksize=1024) as g:
                    buf = np.zeros(4096, dtype=np.float32)
                    while time.monotonic() < self.ativo_ate:
                        bloco = g.record(numframes=2048)[:, 0]
                        buf = np.concatenate([buf[len(bloco):], bloco])
                        mag = np.abs(np.fft.rfft(buf * janela))[faixa]
                        croma = np.bincount(notas, weights=mag, minlength=12)
                        self.croma = croma / croma.max() if croma.max() > 1e-3 else None
            except Exception as e:
                print("Karaokê: erro ao ouvir o PC", repr(e))
                time.sleep(2)


class Karaoke:
    def __init__(self, config):
        self.config = config
        self.status = {"conectado": False}
        self.midia = None
        self.ouvido = Ouvido()
        self.musica = None                  # (título, artista, tocando, posição, duração, quando)
        self.letras = {}                    # id -> [(ms, linha)] (None = buscando)
        self.pontos, self.acerto = 0, 0
        self.ultimo_3ds = 0.0
        self.ultimo_ponto = time.monotonic()

    def iniciar(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", PORTA))
        self.sock.settimeout(1)
        threading.Thread(target=self._musica, daemon=True, name="karaoke-musica").start()
        threading.Thread(target=self._loop, daemon=True, name="karaoke").start()

    @staticmethod
    def id_musica(titulo, artista):
        return zlib.crc32(f"{titulo}\n{artista}".encode("utf-8")) & 0xFFFFFFFF

    def _musica(self):
        """Lê a música 4 vezes por segundo enquanto o karaokê está aberto no 3DS."""
        self.midia = MidiaComTempo()
        while True:
            time.sleep(0.25)
            if time.monotonic() - self.ultimo_3ds > 5:
                continue
            m = self.midia.ler()
            self.musica = (*m, time.monotonic()) if m else None
            if m and m[0]:
                ident = self.id_musica(m[0], m[1])
                if ident not in self.letras:
                    self.letras[ident] = None
                    print(f"Karaokê: buscando a letra de {m[0]} - {m[1]}")
                    threading.Thread(target=lambda: self.letras.__setitem__(ident, buscar_letra(m[0], m[1], m[4])),
                                     daemon=True).start()
                    self.pontos = 0                    # música nova: placar novo

    def _nota_da_voz(self, freq_dhz, volume):
        """Atualiza acerto e pontos com a voz que chegou. Devolve a nota da música."""
        croma = self.ouvido.croma
        alvo = 255 if croma is None else int(np.argmax(croma))
        agora = time.monotonic()
        dt, self.ultimo_ponto = min(0.5, agora - self.ultimo_ponto), agora
        if croma is None or freq_dhz == 0 or volume < 8:
            self.acerto = max(0, self.acerto - 5)
            return alvo
        nota = round(12 * math.log2(freq_dhz / 10 / 440) + 69) % 12
        self.acerto = int(croma[nota] * 100)
        if self.musica and self.musica[2]:
            self.pontos += int(self.acerto * dt * 2)        # até ~200 pontos por segundo cantando certo
        return alvo

    def _loop(self):
        while True:
            try:
                dados, origem = self.sock.recvfrom(64)
            except (socket.timeout, OSError):
                self.status["conectado"] = time.monotonic() - self.ultimo_3ds < 5
                continue
            if len(dados) >= 7 and dados[:4] == b"3DSK":
                if time.monotonic() - self.ultimo_3ds > 5:
                    print(f"Karaokê: 3DS conectado ({origem[0]})")
                self.ultimo_3ds = time.monotonic()
                self.ouvido.ativo_ate = self.ultimo_3ds + 5
                self.status["conectado"] = True
                freq, volume = struct.unpack("<HB", dados[4:7])
                alvo = self._nota_da_voz(freq, volume)
                m = self.musica
                if m:
                    titulo, artista, tocando, pos, _dur, quando = m
                    if tocando:
                        pos += time.monotonic() - quando
                    ident = self.id_musica(titulo, artista)
                    letra = self.letras.get(ident, None)
                    estado_letra = 0 if letra is None else (1 if letra else 2)
                    nome = f"{texto(titulo, 50)}\n{texto(artista, 40)}"
                else:
                    tocando, pos, ident, estado_letra, nome = False, 0, 0, 2, ""
                resposta = b"3DSK" + struct.pack("<BIIBBIB", int(tocando), int(pos * 1000), ident, alvo,
                                                 self.acerto, self.pontos, estado_letra) + nome.encode("utf-8")
                self.sock.sendto(resposta, origem)
            elif len(dados) >= 10 and dados[:4] == b"3DSL":
                ident, parte = struct.unpack("<IH", dados[4:10])
                letra = self.letras.get(ident) or []
                corpo = "".join(f"{ms}\t{linha}\n" for ms, linha in letra).encode("utf-8")
                total = max(1, (len(corpo) + PEDACO - 1) // PEDACO)
                pedaco = corpo[parte * PEDACO:(parte + 1) * PEDACO]
                # não corta uma letra acentuada no meio: o 3DS junta os pedaços antes de ler
                self.sock.sendto(b"3DSL" + struct.pack("<IHH", ident, parte, total) + pedaco, origem)
