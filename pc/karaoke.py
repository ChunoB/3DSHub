"""Karaokê: a letra da música que está tocando no PC rola no 3DS, e a sua voz ganha nota.

- A música é qualquer uma tocando no PC (Spotify, YouTube no navegador...): o nome, o
  artista e em que ponto ela está vêm do Windows (os mesmos controles de mídia da
  Segunda Tela).
- A letra sincronizada vem do lrclib.net ou, se lá não tiver (ou o site estiver fora do
  ar), do NetEase Cloud Music. Grátis, sem cadastro, uma vez por música.
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
import unicodedata
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
from util import SSL

warnings.filterwarnings("ignore", message="data discontinuity")

PORTA = 5016
TAXA = 22050
PEDACO = 900                              # bytes de letra por pacote


LIXO_TITULO = (r"official|oficial|video|vídeo|lyric|letra|audio|áudio|clipe|ao vivo|live|hd|4k|"
               r"remaster|visualizer|\bmv\b")


def normalizar(s):
    """Para comparar nomes: sem acento, sem espaço e pontuação, minúsculo."""
    s = unicodedata.normalize("NFKD", s or "")
    return "".join(c for c in s if c.isalnum()).lower()


def limpar_titulo(titulo, artista):
    """Deixa só "música" e "artista". Títulos do YouTube e do Spotify vêm com sobras:
    "Artista - Música (Official Video)", "Música - Remastered 2011", "Música ft. Fulano",
    e o artista às vezes é o canal ("ArtistaVEVO", "Artista - Topic")."""
    t = re.sub(r"[\(\[][^\)\]]*(" + LIXO_TITULO + r")[^\)\]]*[\)\]]", "", titulo or "", flags=re.I)
    t = re.sub(r"\s+-\s+[^-]*\b(remaster\w*|ao vivo|live|version|versão|edit)\b[^-]*$", "", t, flags=re.I)
    a = re.sub(r"\s*(-\s*topic|vevo|official|oficial)$", "", (artista or "").strip(), flags=re.I)
    if " - " in t:
        esq, dir_ = (x.strip() for x in t.split(" - ", 1))
        na, ne = normalizar(a), normalizar(esq)
        if not na or (ne and (na in ne or ne in na)):    # "Artista - Música" postado pelo artista
            a, t = esq, dir_
    t = re.sub(r"\s*[\(\[]\s*(ft|feat|featuring)\b[^\)\]]*[\)\]]", "", t, flags=re.I)   # "(feat. Fulano)"
    t = re.sub(r"\s+(ft|feat|featuring)\b\.?\s.*$", "", t, flags=re.I)                    # " ft. Fulano"
    return " ".join(t.split()), " ".join(a.split())


def _json(url, cabecalhos):
    req = urllib.request.Request(url, headers=cabecalhos)
    with urllib.request.urlopen(req, timeout=8, context=SSL) as r:
        return json.load(r)


def ler_lrc(lrc):
    """Texto LRC ("[01:02.50]linha") -> [(ms, linha)]. Aceita várias marcas na mesma linha
    e pula os créditos em chinês que o NetEase põe no começo (作词 : ...)."""
    linhas = []
    for linha in (lrc or "").splitlines():
        marcas = re.findall(r"\[(\d+):(\d+(?:\.\d+)?)\]", linha)
        resto = re.sub(r"\[[^\]]*\]", "", linha).strip()
        if re.match(r"^[一-鿿 ]+[:：]", resto):
            continue
        for m, s in marcas:
            linhas.append((int((int(m) * 60 + float(s)) * 1000), texto(resto, 60)))
    linhas.sort()
    return linhas if sum(1 for _, l in linhas if l) >= 5 else []


def _lrclib(t, a, duracao_s):
    cab = {"User-Agent": "3DSHub (github.com/ChunoB/3DSHub)"}
    for params in ({"track_name": t, "artist_name": a}, {"q": f"{a} {t}".strip()}, {"q": t}):
        resultados = _json("https://lrclib.net/api/search?" + urllib.parse.urlencode(params), cab)
        com_letra = [x for x in resultados if x.get("syncedLyrics")]
        if duracao_s:                     # a versão com a duração mais parecida
            com_letra.sort(key=lambda x: abs((x.get("duration") or 0) - duracao_s))
        for x in com_letra[:3]:
            linhas = ler_lrc(x["syncedLyrics"])
            if linhas:
                return linhas
    return []


def _netease(t, a, duracao_s):
    """NetEase Cloud Music: catálogo grande, com muita música ocidental e brasileira."""
    cab = {"User-Agent": "Mozilla/5.0", "Referer": "https://music.163.com/"}
    r = _json("https://music.163.com/api/search/get?" +
              urllib.parse.urlencode({"s": f"{t} {a}".strip(), "type": 1, "limit": 10}), cab)
    nt, na = normalizar(t), normalizar(a)

    def confere(s):                       # mesmo nome e mesmo artista (não um cover)
        nome = normalizar(s.get("name"))
        artistas = [normalizar(x.get("name")) for x in s.get("artists") or []]
        return (nome and (nt in nome or nome in nt)) and \
               (not na or any(x and (x in na or na in x) for x in artistas))

    musicas = [s for s in (r.get("result") or {}).get("songs") or [] if confere(s)]
    if duracao_s:
        musicas.sort(key=lambda s: abs((s.get("duration") or 0) / 1000 - duracao_s))
    for s in musicas[:3]:
        letra = _json(f"https://music.163.com/api/song/lyric?id={s['id']}&lv=1", cab)
        linhas = ler_lrc((letra.get("lrc") or {}).get("lyric"))
        if linhas:
            return linhas
    return []


FONTES = (("lrclib", _lrclib), ("NetEase", _netease))


def buscar_letra(titulo, artista, duracao_s):
    """[(ms, linha)] da letra sincronizada; [] se nenhuma fonte tem; None se alguma fonte
    deu erro (site fora do ar, sem internet) e vale tentar de novo daqui a pouco."""
    t, a = limpar_titulo(titulo, artista)
    if not t:
        return []
    erro = False
    for nome, fonte in FONTES:
        try:
            linhas = fonte(t, a, duracao_s)
        except Exception as e:
            print(f"Karaokê: {nome} falhou ({e!r})")
            erro = True
            continue
        if linhas:
            print(f"Karaokê: letra de {t} - {a} achada no {nome}")
            return linhas
    return None if erro else []


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
        self.reprocurar = {}                # id -> (tentativas, quando buscar de novo; 0 = não)
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
                tentativas, quando = self.reprocurar.get(ident, (0, 0.0))
                if ident not in self.letras:
                    self.pontos = 0                    # música nova: placar novo
                    self._procurar(ident, m, 0)
                elif quando and time.monotonic() > quando:
                    self._procurar(ident, m, tentativas)

    def _procurar(self, ident, m, tentativas):
        """Busca a letra numa thread. Se um site falhou (fora do ar, sem internet), tenta de
        novo em 30 s, 1, 2 e 4 min enquanto a música tocar, em vez de ficar "sem letra"."""
        self.letras[ident] = None
        self.reprocurar[ident] = (tentativas, 0.0)
        print(f"Karaokê: {'buscando' if not tentativas else 'tentando de novo'} a letra de {m[0]} - {m[1]}")

        def trabalho():
            linhas = buscar_letra(m[0], m[1], m[4])
            if linhas is None and tentativas < 4:
                self.reprocurar[ident] = (tentativas + 1, time.monotonic() + 30 * 2 ** tentativas)
            self.letras[ident] = linhas or []

        threading.Thread(target=trabalho, daemon=True).start()

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
