"""Karaokê: a letra da música que está tocando no PC rola no 3DS, e a sua voz ganha nota.

- A música é qualquer uma tocando no PC (Spotify, YouTube no navegador...): o nome, o
  artista e em que ponto ela está vêm do Windows (os mesmos controles de mídia da
  Segunda Tela).
- A letra sincronizada vem do lrclib.net ou, se lá não tiver (ou o site estiver fora do
  ar), do NetEase Cloud Music. Grátis, sem cadastro, uma vez por música.
- A nota: o 3DS descobre a nota que você está cantando (microfone) e manda só ela. O PC
  ouve o som que está tocando (loopback), vê quais notas a música está tocando agora
  (cromagrama) e compara. Oitava não importa: cantar uma oitava abaixo vale igual.
- A faixa (estilo Guitar Hero): o PC grava a nota da música a cada 100 ms enquanto ela
  toca (karaoke/notas/<id>.bin). Na primeira vez a faixa mostra os trechos de canto (pelo
  tempo da letra) e as notas aparecem na hora em que soam; da segunda vez em diante as
  notas gravadas vêm vindo pela faixa antes de tocar.
- O jogo: cada nota da música vale Perfeito / Ótimo / Bom / Errou; combo sobe o
  multiplicador (x1 a x4) e o Turbo (A no 3DS, quando cheio) dobra os pontos por 10 s.
  Perfis, vez de cada jogador (a mesma música recomeça para o próximo) e recordes ficam
  em karaoke/perfis.json.

Protocolo (UDP 5016):
    3DS -> "3DSK" + u16 frequência (décimos de Hz; 0 = sem voz) + u8 volume (0-100)
           [+ u8 flags: 1 = apertou o Turbo, 2 = 3DS novo (quer a resposta "3DSG")]
    PC  -> "3DSK" (3DS antigo) + u8 tocando + u32 posição (ms) + u32 id da música + u8 nota
           da música (0 = Dó ... 11 = Si; 255 = nenhuma) + u8 acerto agora (0-100)
           + u32 pontos + u8 estado da letra (0 buscando, 1 achou, 2 sem letra)
           + texto "título\\nartista"
    PC  -> "3DSG" (3DS novo): veja Karaoke._estado_jogo
    3DS -> "3DSL" + u32 id + u16 parte          pede um pedaço da letra
    PC  -> "3DSL" + u32 id + u16 parte + u16 total + texto ("ms\\tlinha\\n"...)
    3DS -> "3DSP" + u8 seq + u8 comando + dados  perfis e rodadas (veja Karaoke._comando)
    PC  -> "3DSP" + u8 seq + u8 comando + texto
"""

import asyncio
import json
import math
import os
import queue
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

from config import PASTA
from tela import texto
from util import SSL

warnings.filterwarnings("ignore", message="data discontinuity")

PORTA = 5016
TAXA = 22050
PEDACO = 900                              # bytes de letra por pacote

# faixa de notas
SLOT = 0.1                                # segundos por casa da faixa
ANTES, DEPOIS = 12, 36                    # casas mandadas para o 3DS: 1,2 s atrás e 3,6 s à frente
SEM_NOTA, CANTO, NAO_GRAVADO = 255, 252, 253
INSTRUMENTAL = 100                        # nota + 100: nota fora dos trechos com letra (não vale)
PASTA_K = os.path.join(PASTA, "karaoke")

# jogo
PERFEITO, OTIMO, BOM, ERROU = 1, 2, 3, 4
PONTOS_JULG = {PERFEITO: 100, OTIMO: 70, BOM: 40, ERROU: 0}
NOTA_MIN_S = 0.25                         # notas mais curtas não são julgadas
LATENCIA_VOZ = 0.12                       # a voz chega ao PC ~0,12 s depois de cantada (microfone + rede)
FACIL, NORMAL, DIFICIL = 0, 1, 2
NOMES_DIF = {FACIL: "F", NORMAL: "N", DIFICIL: "D"}
# por dificuldade: crédito pela distância em semitons (0, 1, 2); "vale se a nota está forte na música
# agora" (croma mínimo; None = não vale); limites de Perfeito / Ótimo / Bom
REGRAS = {FACIL: ((1.0, 1.0, 0.5), 0.6, (0.65, 0.4, 0.2)),
          NORMAL: ((1.0, 0.75, 0.0), 0.85, (0.7, 0.45, 0.25)),
          DIFICIL: ((1.0, 0.0, 0.0), None, (0.8, 0.55, 0.3))}
TURBO_S = 10
MAX_JOGADORES = 4


LIXO_TITULO = (r"official|oficial|video|vídeo|lyric|letra|audio|áudio|clipe|ao vivo|live|hd|4k|"
               r"remaster|visualizer|\bmv\b|\bdvd\b|kondzilla")


def sem_acento(s):
    return "".join(c for c in unicodedata.normalize("NFKD", s or "") if not unicodedata.combining(c))


def normalizar(s):
    """Para comparar nomes: sem acento, sem espaço e pontuação, minúsculo, e "&" = "e" = "and"
    ("Henrique & Juliano" e "Henrique e Juliano" ficam iguais)."""
    s = re.sub(r"(^|\W)(&|e|and|y)(?=\W|$)", " ", sem_acento(s).lower())
    return "".join(c for c in s if c.isalnum())


def nomes_de(nome):
    """O nome inteiro e o nome sem "(Ao Vivo)", "[Remix]" ou "- Remastered" (normalizados):
    "Infiel (Ao Vivo)" vale como "Infiel", mas "Infiel / Amante Não Tem Lar" não."""
    base = re.sub(r"\s*[\(\[][^\)\]]*[\)\]]|\s+-\s.*$", "", nome or "")
    return {x for x in (normalizar(nome), normalizar(base)) if x}


def artistas_de(a):
    """"Anitta, MC Zaac feat. Fulano" -> ["Anitta", "MC Zaac", "Fulano"] ("&" fica: é dupla)."""
    return [x.strip() for x in re.split(r",|\b(?:feat|ft|featuring)\b\.?", a or "", flags=re.I) if x.strip()]


def limpar_titulo(titulo, artista):
    """Deixa só "música" e "artista". Títulos do YouTube e do Spotify vêm com sobras:
    "Artista - Música (Official Video)", "Música - Remastered 2011", "Música ft. Fulano",
    e o artista às vezes é o canal ("ArtistaVEVO", "Artista - Topic")."""
    t = re.sub(r"\s+[–—]\s+", " - ", titulo or "")                    # travessão = hífen
    t = re.sub(r"[\(\[][^\)\]]*(" + LIXO_TITULO + r")[^\)\]]*[\)\]]", "", t, flags=re.I)
    t = re.sub(r"\s+M/?V\.?$", "", t, flags=re.I)                     # "Música M/V"
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
            linhas.append((int((int(m) * 60 + float(s)) * 1000), texto(resto, 160)))   # inteira: o 3DS quebra em linhas
    linhas.sort()
    return linhas if sum(1 for _, l in linhas if l) >= 5 else []


def _lrclib(t, a, duracao_s):
    cab = {"User-Agent": "3DSHub (github.com/ChunoB/3DSHub)"}
    a = (artistas_de(a) or [""])[0]       # só o artista principal: a lista toda atrapalha a busca
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
    """NetEase Cloud Music: catálogo grande, com muita música ocidental e brasileira.
    A mesma música aparece várias vezes lá (álbum, coletânea, ao vivo) e muitas vezes só uma
    das cópias tem a letra sincronizada: olha várias, a de nome igual primeiro, e busca de novo
    sem acento (as cópias sem acento costumam ser outras)."""
    cab = {"User-Agent": "Mozilla/5.0", "Referer": "https://music.163.com/"}
    nt = normalizar(t)
    nomes_t = nomes_de(t)
    pedidos = [normalizar(x) for x in artistas_de(a)]
    principal = (artistas_de(a) or [""])[0]

    def confere(s):                       # mesmo nome e um dos artistas (não um cover nem pot-pourri)
        artistas = [normalizar(x.get("name")) for x in s.get("artists") or []]
        return bool(nomes_t & nomes_de(s.get("name"))) and \
               (not pedidos or any(x and p and (x in p or p in x) for x in artistas for p in pedidos))

    def ordem(s):                         # nome igual (não "ao vivo", "remix"), depois a duração
        return (normalizar(s.get("name")) != nt,
                abs((s.get("duration") or 0) / 1000 - duracao_s) if duracao_s else 0)

    busca = f"{t} {principal}".strip()
    vistos = set()
    for q in dict.fromkeys((busca, sem_acento(busca))):
        r = _json("https://music.163.com/api/search/get?" +
                  urllib.parse.urlencode({"s": q, "type": 1, "limit": 20}), cab)
        musicas = [s for s in (r.get("result") or {}).get("songs") or [] if s["id"] not in vistos and confere(s)]
        vistos.update(s["id"] for s in musicas)
        for s in sorted(musicas, key=ordem)[:6]:
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
    pares = [(t, a)]
    if " - " in t:                        # "Artista - Música" postado por um canal (ex.: KondZilla)
        esq, dir_ = (x.strip() for x in t.split(" - ", 1))
        pares.append((dir_, esq))
    erro = False
    for nome, fonte in FONTES:
        for t, a in pares:
            try:
                linhas = fonte(t, a, duracao_s)
            except Exception as e:
                print(f"Karaokê: {nome} falhou ({e!r})")
                erro = True
                break                     # o site está com problema: não adianta a outra forma
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

    async def _comando(self, qual):
        if self.gerenciador is None:
            self.gerenciador = await self.G.request_async()
        s = self.gerenciador.get_current_session()
        if not s:
            return False
        if qual == "pausar":
            return await s.try_pause_async()
        if qual == "proxima":
            return await s.try_skip_next_async()
        if qual == "anterior":                # volta para a música de antes (o Spotify: no
            ok = await s.try_skip_previous_async()     # comecinho da música, "anterior" volta uma)
            await s.try_play_async()
            return ok
        if qual == "reiniciar":               # do começo; quem não deixa mudar a posição recomeça
            ok = await s.try_change_playback_position_async(0)     # com "anterior", mas só longe do
            if not ok and s.get_timeline_properties().position.total_seconds() > 4:   # começo: ali o
                ok = await s.try_skip_previous_async()             # "anterior" volta uma música
            await s.try_play_async()
            return ok
        return False

    def comando(self, qual):
        """Manda um comando para o player (só nesta thread: o loop do asyncio é dela)."""
        try:
            ok = self.loop.run_until_complete(self._comando(qual))
        except Exception as e:
            print("Karaokê: o player não aceitou", qual, repr(e))
            self.gerenciador = None
            return False
        if not ok:
            print("Karaokê: o player não aceitou", qual)
        return ok


class Ouvido:
    """Escuta o som do PC e guarda as 12 notas (Dó a Si) que mais soam agora."""

    def __init__(self):
        self.croma = None
        self.ativo_ate = 0.0
        self.forte = False                  # tem som de verdade (não silêncio nem chiado)
        self.pico = 1e-3                    # o mais alto dos últimos segundos (para comparar)
        if soundcard:
            threading.Thread(target=self._loop, daemon=True, name="karaoke-ouvido").start()

    def _loop(self):
        import ctypes
        ctypes.windll.ole32.CoInitializeEx(None, 0)          # o som do Windows usa COM
        freqs = np.fft.rfftfreq(4096, 1 / TAXA)
        faixa = (freqs > 150) & (freqs < 1100)      # região da voz (menos baixo e bumbo)
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
                        maior = float(croma.max())
                        self.pico = max(maior, self.pico * 0.995)
                        self.croma = croma / maior if maior > 1e-3 else None
                        # nota clara: som alto o bastante e uma nota se destacando (não um chiado)
                        self.forte = self.croma is not None and maior > self.pico * 0.08 \
                            and float(self.croma.mean()) < 0.6
            except Exception as e:
                print("Karaokê: erro ao ouvir o PC", repr(e))
                time.sleep(2)


def suavizar(faixa):
    """Faixa crua -> faixa para mostrar e julgar: tira as notas soltas (uma casa só), que
    são acordes e batidas trocando a nota mais forte por um instante."""
    v = list(faixa)
    n = len(v)
    out = v[:]
    for i in range(1, n - 1):                 # uma casa diferente entre duas iguais vira igual
        if v[i - 1] == v[i + 1] and v[i - 1] < 12 and v[i] != v[i - 1] and v[i] != NAO_GRAVADO:
            out[i] = v[i - 1]
    i = 0
    while i < n:                              # nota de uma casa só (100 ms) vira "sem nota"
        j = i
        while j < n and out[j] == out[i]:
            j += 1
        if out[i] < 12 and j - i < 2:
            for k in range(i, j):
                out[k] = SEM_NOTA
        i = j
    return bytes(out)


class Notas:
    """As notas de cada música, gravadas enquanto ela toca (uma casa a cada 100 ms) e salvas
    em karaoke/notas/<id>.bin. NAO_GRAVADO = essa parte ainda não foi ouvida."""

    def __init__(self):
        self.pasta = os.path.join(PASTA_K, "notas")
        os.makedirs(self.pasta, exist_ok=True)
        self.faixas, self.suaves, self.coberturas = {}, {}, {}
        self.mudou = set()
        self.trava = threading.Lock()

    def faixa(self, ident, dur):
        with self.trava:
            f = self.faixas.get(ident)
            if f is None:
                try:
                    with open(os.path.join(self.pasta, f"{ident:08x}.bin"), "rb") as arq:
                        f = bytearray(arq.read())
                except OSError:
                    f = bytearray()
                self.faixas[ident] = f
                self.suaves[ident] = suavizar(f)
                self.coberturas[ident] = self._cobertura(f)
            n = int(dur / SLOT) + 1 if dur > 0 else 0
            if len(f) < n:
                f.extend([NAO_GRAVADO] * (n - len(f)))
            return f

    @staticmethod
    def _cobertura(f):
        return sum(1 for x in f if x != NAO_GRAVADO) / len(f) if f else 0.0

    def gravar(self, ident, dur, pos, nota):
        f = self.faixa(ident, dur)
        i = int(pos / SLOT)
        if 0 <= i < len(f) and f[i] == NAO_GRAVADO:
            f[i] = nota
            self.mudou.add(ident)

    def suave(self, ident):
        return self.suaves.get(ident, b"")

    def cobertura(self, ident):
        return self.coberturas.get(ident, 0.0)

    def salvar(self):
        with self.trava:
            ids, self.mudou = list(self.mudou), set()
            copias = {i: bytes(self.faixas[i]) for i in ids}
        for ident, f in copias.items():
            self.suaves[ident] = suavizar(f)
            self.coberturas[ident] = self._cobertura(f)
            arquivo = os.path.join(self.pasta, f"{ident:08x}.bin")
            try:
                with open(arquivo + ".tmp", "wb") as arq:
                    arq.write(f)
                os.replace(arquivo + ".tmp", arquivo)
            except OSError as e:
                print("Karaokê: não consegui salvar as notas", repr(e))


def nivel(xp):
    """(nível, % até o próximo). Cada nível pede 500 XP a mais que o anterior."""
    n = 1
    while 250 * n * (n + 1) <= xp:
        n += 1
    base, prox = 250 * (n - 1) * n, 250 * n * (n + 1)
    return n, int(100 * (xp - base) / (prox - base))


class Perfis:
    """Perfis (nome, cor, XP) e recordes de cada música, em karaoke/perfis.json."""

    MAX = 12

    def __init__(self):
        self.arquivo = os.path.join(PASTA_K, "perfis.json")
        os.makedirs(PASTA_K, exist_ok=True)
        self.trava = threading.Lock()
        try:
            with open(self.arquivo, encoding="utf-8") as arq:
                self.dados = json.load(arq)
        except (OSError, ValueError):
            self.dados = {}
        self.dados.setdefault("perfis", [])
        self.dados.setdefault("recordes", {})
        self.dados.setdefault("proximo", 1)

    def _salvar(self):
        try:
            with open(self.arquivo + ".tmp", "w", encoding="utf-8") as arq:
                json.dump(self.dados, arq, ensure_ascii=False, indent=1)
            os.replace(self.arquivo + ".tmp", self.arquivo)
        except OSError as e:
            print("Karaokê: não consegui salvar os perfis", repr(e))

    def lista(self):
        return list(self.dados["perfis"])

    def achar(self, pid):
        return next((p for p in self.dados["perfis"] if p["id"] == pid), None)

    def criar(self, nome, cor):
        nome = " ".join(nome.split())[:16]
        with self.trava:
            if not nome or len(self.dados["perfis"]) >= self.MAX:
                return None
            usados = {p["id"] for p in self.dados["perfis"]}
            pid = self.dados["proximo"]
            while pid in usados or not 1 <= pid <= 250:          # o id vai num byte para o 3DS
                pid = pid % 250 + 1
            self.dados["proximo"] = pid % 250 + 1
            p = {"id": pid, "nome": nome, "cor": cor % 8, "xp": 0, "partidas": 0}
            self.dados["perfis"].append(p)
            self._salvar()
            print(f"Karaokê: perfil criado: {nome}")
            return p

    def apagar(self, pid):
        with self.trava:
            self.dados["perfis"] = [p for p in self.dados["perfis"] if p["id"] != pid]
            self._salvar()

    def top(self, ident):
        return self.dados["recordes"].get(f"{ident:08x}", {}).get("top", [])

    def registrar(self, pid, ident, titulo, artista, res):
        """Guarda o resultado da vez: XP do perfil e recordes da música."""
        info = {"recorde": False, "recorde_geral": False, "subiu": False, "xp": 0, "nivel": 0}
        with self.trava:
            p = self.achar(pid)
            if not p:
                return info
            xp = 50 + res["pontos"] // 20
            antes = nivel(p["xp"])[0]
            p["xp"] += xp
            p["partidas"] += 1
            depois = nivel(p["xp"])[0]
            info.update(xp=xp, nivel=depois, subiu=depois > antes)
            rec = self.dados["recordes"].setdefault(f"{ident:08x}", {"titulo": titulo, "artista": artista, "top": []})
            top = rec["top"]
            if res["pontos"] > 0:
                meu = max((x["pontos"] for x in top if x["perfil"] == pid), default=-1)
                info["recorde"] = res["pontos"] > meu
                info["recorde_geral"] = not top or res["pontos"] > top[0]["pontos"]
                top.append({"perfil": pid, "nome": p["nome"], "pontos": res["pontos"],
                            "estrelas": res["estrelas"], "precisao": res["precisao"],
                            "dif": res.get("dificuldade", NORMAL),
                            "data": time.strftime("%Y-%m-%d")})
                top.sort(key=lambda x: -x["pontos"])
                del top[5:]
            self._salvar()
        return info


class Vez:
    """A vez de um jogador numa música: julga cada nota e soma pontos, combo e Turbo."""

    def __init__(self, pid, nome, cor, dificuldade=NORMAL):
        self.pid, self.nome, self.cor, self.dif = pid, nome, cor, dificuldade
        self.pontos = self.combo = self.combo_max = 0
        self.contagem = {PERFEITO: 0, OTIMO: 0, BOM: 0, ERROU: 0}
        self.turbo, self.turbo_ate = 0.0, 0.0
        self.soma_acerto = self.soma_dur = 0.0
        self.julg = self.julg_seq = 0
        self.nota = None                      # [nota, duração, acerto, tempo com voz] da nota tocando
        self.tocado = 0.0                     # segundos de música tocando nesta vez
        self.so_com_voz = False
        self.resultado, self.info = None, {}

    def mult(self):
        return 1 + min(3, self.combo // 8)

    def passo(self, alvo, minha, dt, tocando, agora, croma=None, so_com_voz=False):
        """alvo = nota da música agora (None = nenhuma); minha = nota da voz (None = calado);
        croma = as 12 notas soando na música agora (None = não sabe). Devolve o acerto deste
        instante (0 a 100). Pausado não conta nem fecha a nota. so_com_voz = música sem letra:
        nota em que a pessoa ficou calada não conta (nem como erro)."""
        if not tocando:
            return 0
        self.tocado += dt
        self.so_com_voz = so_com_voz
        if self.nota and alvo != self.nota[0]:
            self._julgar(agora)
        if alvo is None:
            return 0
        if not self.nota:
            self.nota = [alvo, 0.0, 0.0, 0.0]
        creditos, croma_min, _ = REGRAS[self.dif]
        q = 0.0
        if minha is not None:
            d = abs(minha - alvo) % 12
            d = min(d, 12 - d)
            q = creditos[d] if d <= 2 else 0.0
            # a "nota da música" é a mais forte da mistura (às vezes um instrumento): cantar outra
            # nota que está soando forte agora (a melodia, uma harmonia) também vale
            if croma_min is not None and croma is not None and croma[minha] >= croma_min:
                q = 1.0
        self.nota[1] += dt
        if minha is not None:
            self.nota[3] += dt
        self.nota[2] += q * dt
        return int(q * 100)

    def _julgar(self, agora):
        _, dur, acerto, com_voz = self.nota
        self.nota = None
        if dur < NOTA_MIN_S or (self.so_com_voz and com_voz < 0.15 * dur):
            return
        frac = acerto / dur
        perf, otimo, bom = REGRAS[self.dif][2]
        j = PERFEITO if frac >= perf else OTIMO if frac >= otimo else BOM if frac >= bom else ERROU
        self.contagem[j] += 1
        self.soma_acerto += frac * dur
        self.soma_dur += dur
        self.combo = self.combo + 1 if j != ERROU else 0
        self.combo_max = max(self.combo_max, self.combo)
        turbo = agora < self.turbo_ate
        self.pontos += int(PONTOS_JULG[j] * min(4.0, max(1.0, dur / 0.5)) * self.mult() * (2 if turbo else 1))
        if j == PERFEITO and not turbo:
            self.turbo = min(100.0, self.turbo + 12.5)
        self.julg, self.julg_seq = j, (self.julg_seq + 1) % 256

    def ativar_turbo(self, agora):
        if self.turbo >= 100 and agora >= self.turbo_ate:
            self.turbo, self.turbo_ate = 0.0, agora + TURBO_S

    def fechar(self, agora):
        if self.nota:
            self._julgar(agora)
        precisao = round(100 * self.soma_acerto / self.soma_dur) if self.soma_dur else 0
        estrelas = next((e for e, minimo in ((5, 90), (4, 75), (3, 60), (2, 40), (1, 15)) if precisao >= minimo), 0)
        self.resultado = {"pontos": self.pontos, "precisao": precisao, "estrelas": estrelas,
                          "combo_max": self.combo_max, "contagem": dict(self.contagem),
                          "dificuldade": self.dif}
        return self.resultado


class Sessao:
    """Uma rodada: 1 a 4 jogadores cantando a mesma música, um de cada vez."""
    CANTANDO, FIM_DA_VEZ, FIM = 1, 2, 3

    def __init__(self, jogadores, musica, dificuldade=NORMAL):
        self.jogadores = jogadores            # [(id do perfil ou 0, nome, cor)]
        self.musica = musica                  # (id, título, artista) ou None (pega a próxima)
        self.dif = dificuldade
        self.vezes = [Vez(*jogadores[0], dificuldade)]
        self.estado = self.CANTANDO
        self.desde = time.monotonic()         # a música acabou de (re)começar: espera assentar
        self.outra_desde = None               # desde quando o player mostra outra música

    @property
    def vez(self):
        return self.vezes[-1]

    def proxima(self):
        self.vezes.append(Vez(*self.jogadores[len(self.vezes)], self.dif))
        self.estado, self.desde, self.outra_desde = self.CANTANDO, time.monotonic(), None


class Karaoke:
    def __init__(self, config):
        self.config = config
        self.status = {"conectado": False}
        self.midia = None
        self.ouvido = Ouvido()
        self.musica = None                  # (título, artista, tocando, posição, duração, quando)
        self.letras = {}                    # id -> [(ms, linha)] (None = buscando)
        self.reprocurar = {}                # id -> (tentativas, quando buscar de novo; 0 = não)
        self.pontos, self.acerto = 0, 0     # do modo antigo (3DS sem a versão nova)
        self.ultimo_3ds = 0.0
        self.ultimo_ponto = time.monotonic()
        self.notas = Notas()
        self.perfis = Perfis()
        self.sessao = None
        self.trava = threading.RLock()
        self.comandos = queue.Queue()       # para o player (só a thread da música fala com ele)
        self.alvos = []                     # últimas notas mais fortes (para não pular à toa)
        self.canto = {}                     # id -> bytes: 1 nas casas em que tem letra
        self.ultimo_salvar = time.monotonic()

    def iniciar(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", PORTA))
        self.sock.settimeout(1)
        threading.Thread(target=self._musica, daemon=True, name="karaoke-musica").start()
        threading.Thread(target=self._loop, daemon=True, name="karaoke").start()

    @staticmethod
    def id_musica(titulo, artista):
        return zlib.crc32(f"{titulo}\n{artista}".encode("utf-8")) & 0xFFFFFFFF

    @staticmethod
    def _posicao(m):
        _titulo, _artista, tocando, pos, _dur, quando = m
        return pos + (time.monotonic() - quando if tocando else 0)

    # ------------------------------------------------ música (thread própria)
    def _musica(self):
        """Lê a música 4 vezes por segundo enquanto o karaokê está aberto no 3DS, cuida da
        vez de cada jogador e manda os comandos para o player."""
        self.midia = MidiaComTempo()
        while True:
            try:
                qual = self.comandos.get(timeout=0.25)
            except queue.Empty:
                qual = None
            if qual:
                self.midia.comando(qual)
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
            self._vigiar_vez()
            if time.monotonic() - self.ultimo_salvar > 10:
                self.ultimo_salvar = time.monotonic()
                self.notas.salvar()

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

    def _vigiar_vez(self):
        """A vez acaba quando a música chega ao fim (aí pausa, para o player não pular para
        a próxima) ou quando outra música começa."""
        with self.trava:
            s, m = self.sessao, self.musica
            if not s or s.estado != Sessao.CANTANDO or not m or not m[0]:
                return
            ident = self.id_musica(m[0], m[1])
            if s.musica is None:
                s.musica = (ident, m[0], m[1])
                return
            agora = time.monotonic()
            if agora - s.desde < 4:                     # recomeçando: a posição ainda é a velha
                return
            if ident != s.musica[0]:
                # o player mostra outra música por um instante quando carrega a próxima: só
                # encerra se continuar assim
                if s.outra_desde is None:
                    s.outra_desde = agora
                elif agora - s.outra_desde > 3:
                    self._fim_da_vez()
                return
            s.outra_desde = None
            dur, pos = m[4], self._posicao(m)
            # fim da música: só se ela tocou de verdade nesta vez (quando a música troca, a duração
            # e a posição que o Windows dá às vezes ficam velhas por um tempo)
            if dur > 5 and dur - 1.2 <= pos <= dur + 5 and s.vez.tocado >= min(dur * 0.6, dur - 10):
                self._fim_da_vez()
                self.comandos.put("pausar")

    def _fim_da_vez(self):
        s = self.sessao
        v = s.vez
        res = v.fechar(time.monotonic())
        if v.pid and s.musica:
            v.info = self.perfis.registrar(v.pid, *s.musica, res)
        s.estado = Sessao.FIM if len(s.vezes) >= len(s.jogadores) else Sessao.FIM_DA_VEZ
        self.notas.salvar()
        print(f"Karaokê: {v.nome} fez {res['pontos']} pontos ({res['precisao']}%, {res['estrelas']} estrelas)")

    def _voltar_musica(self):
        """Para a próxima vez: a mesma música do começo."""
        s, m = self.sessao, self.musica
        mesma = s.musica is None or (m and m[0] and self.id_musica(m[0], m[1]) == s.musica[0])
        self.comandos.put("reiniciar" if mesma else "anterior")

    # ------------------------------------------------ nota da música e da voz
    def _nota_da_voz(self, freq_dhz, volume):
        """Modo antigo (3DS sem a versão nova): acerto e pontos com a voz que chegou."""
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

    def _jogar(self, freq, volume, flags, m, pos, ident):
        """3DS novo: grava a nota da música, acha a nota que vale agora e julga a voz."""
        agora = time.monotonic()
        dt, self.ultimo_ponto = min(0.2, agora - self.ultimo_ponto), agora
        tocando = bool(m and m[2])
        croma = self.ouvido.croma if soundcard else None
        viva = None
        if croma is not None and self.ouvido.forte:
            self.alvos = (self.alvos + [int(np.argmax(croma))])[-3:]
            viva = max(set(self.alvos), key=self.alvos.count)
        else:
            self.alvos = []
        alvo = viva
        if m and ident and m[4] > 0:
            if tocando and croma is not None:          # o som chega ~0,12 s atrasado (janela do FFT)
                self.notas.gravar(ident, m[4], pos - 0.12, SEM_NOTA if viva is None else viva)
            s, i = self.notas.suave(ident), int((pos - LATENCIA_VOZ) / SLOT)    # a voz chega atrasada
            if 0 <= i < len(s) and s[i] != NAO_GRAVADO:   # já gravada: vale a da faixa (a que o 3DS mostra)
                alvo = None if s[i] == SEM_NOTA else s[i]
        canto = self._canto_de(ident, m[4]) if m and ident else b""
        if canto:                                       # parte instrumental (sem letra): não vale
            j = int((pos - LATENCIA_VOZ) / SLOT)
            if not (0 <= j < len(canto) and canto[j]):
                alvo = None
        minha = None
        if freq and volume >= 8:
            minha = round(12 * math.log2(freq / 10 / 440) + 69) % 12
        acerto = 0
        with self.trava:
            s = self.sessao
            if s and s.estado == Sessao.CANTANDO and s.musica and ident == s.musica[0]:
                if flags & 1:
                    s.vez.ativar_turbo(agora)
                acerto = s.vez.passo(alvo, minha, dt, tocando, agora, croma if self.ouvido.forte else None,
                                     so_com_voz=not canto)
        return alvo, acerto

    def _canto_de(self, ident, dur):
        """Casas em que tem letra (a faixa mostra "vai ter canto aqui" antes de saber a nota)."""
        letra = self.letras.get(ident)
        if not letra:
            return b""
        c = self.canto.get(ident)
        if c is None:
            n = int(max(dur, letra[-1][0] / 1000 + 5) / SLOT) + 1
            arr = bytearray(n)
            for k, (ms, linha) in enumerate(letra):
                if not linha.strip():
                    continue
                palavras = len(linha.split())       # ~0,55 s por palavra (e um respiro)
                fim = min(letra[k + 1][0] if k + 1 < len(letra) else ms + 5000,
                          ms + max(2500, 550 * palavras + 1200), ms + 8000)
                for i in range(ms // 100, min(n, fim // 100)):
                    arr[i] = 1
            c = self.canto[ident] = bytes(arr)
        return c

    def _janela(self, ident, dur, pos):
        """As casas da faixa de 1,2 s atrás a 3,6 s à frente: a nota gravada, o que acabou de
        soar, ou CANTO (tem letra, nota ainda desconhecida). Com letra, nota fora dos trechos
        cantados vai como nota + INSTRUMENTAL (aparece em cinza e não vale)."""
        atual = int(pos / SLOT)
        crua = self.notas.faixas.get(ident) or b""
        suave = self.notas.suave(ident)
        canto = self._canto_de(ident, dur)
        out = bytearray()
        for i in range(atual - ANTES, atual + DEPOIS):
            v = NAO_GRAVADO
            if 0 <= i < len(suave) and suave[i] != NAO_GRAVADO:
                v = suave[i]
            elif i <= atual and 0 <= i < len(crua) and crua[i] != NAO_GRAVADO:
                v = crua[i]
            elif 0 <= i < len(canto) and canto[i]:
                v = CANTO
            if v < 12 and canto and not (0 <= i < len(canto) and canto[i]):
                v += INSTRUMENTAL
            out.append(v)
        return bytes(out)

    def _estado_jogo(self, m, pos, ident, alvo, acerto):
        """'3DSG' + u8 tocando + u32 posição (ms) + u32 id + u8 estado da letra + u8 notas gravadas
        (0/1) + u8 nota da música agora (255 = nenhuma) + u8 nº do julgamento (muda a cada um)
        + u8 julgamento (1 Perfeito, 2 Ótimo, 3 Bom, 4 Errou) + u32 pontos + u16 combo
        + u8 multiplicador + u8 Turbo (0-100) + u8 segundos de Turbo ligado + u8 rodada (0 sem,
        1 cantando, 2 fim da vez, 3 fim da rodada) + u8 vez + u8 jogadores + u8 acerto agora
        + u8 progresso da música (0-100) + 48 casas da faixa + "título\\nartista\\njogador"."""
        agora = time.monotonic()
        if m:
            titulo, artista, tocando, dur = m[0], m[1], m[2], m[4]
            letra = self.letras.get(ident)
            estado_letra = 0 if letra is None else (1 if letra else 2)
            gravada = int(self.notas.cobertura(ident) >= 0.8)
            janela = self._janela(ident, dur, pos)
            progresso = int(min(100, 100 * pos / dur)) if dur > 0 else 0
        else:
            titulo = artista = ""
            tocando, estado_letra, gravada, progresso = False, 2, 0, 0
            janela = bytes([NAO_GRAVADO]) * (ANTES + DEPOIS)
        with self.trava:
            s = self.sessao
            if s:
                v = s.vez
                rodada, vez, n, jogador = s.estado, len(s.vezes) - 1, len(s.jogadores), v.nome
                julg_seq, julg, pontos, combo, mult = v.julg_seq, v.julg, v.pontos, v.combo, v.mult()
                turbo, turbo_s = int(v.turbo), max(0, int(math.ceil(v.turbo_ate - agora)))
            else:
                rodada = vez = n = julg_seq = julg = pontos = combo = turbo = turbo_s = 0
                mult, jogador = 1, ""
        cab = struct.pack("<BIIBBBBBIHBBBBBBBB", int(tocando), int(pos * 1000), ident, estado_letra, gravada,
                          255 if alvo is None else alvo, julg_seq, julg, pontos, min(combo, 65535), mult,
                          turbo, min(turbo_s, 255), rodada, vez, n, acerto, progresso)
        nome = f"{texto(titulo, 50)}\n{texto(artista, 40)}\n{texto(jogador, 16)}"
        return b"3DSG" + cab + janela + nome.encode("utf-8")

    # ------------------------------------------------ perfis e rodadas
    def _lista(self):
        linhas = []
        for p in self.perfis.lista():
            nv, pct = nivel(p["xp"])
            linhas.append(f"{p['id']}\t{texto(p['nome'], 16)}\t{p['cor']}\t{nv}\t{pct}\t{p['partidas']}\n")
        return "".join(linhas)

    def _tops(self, ident):
        return "".join(f"T\t{texto(x['nome'], 16)}\t{x['pontos']}\t{x['estrelas']}\t"
                       f"{NOMES_DIF.get(x.get('dif', NORMAL), 'N')}\n" for x in self.perfis.top(ident))

    def _comando(self, cmd, dados):
        """3DSP: 'L' lista os perfis ("id\\tnome\\tcor\\tnível\\t%\\tpartidas" por linha);
        'N' + u8 cor + nome cria; 'A' + u8 id apaga; 'S' + u8 n + ids (0 = convidado) [+ u8
        dificuldade: 0 fácil, 1 normal, 2 difícil] começa a rodada (a música recomeça); 'P' próximo jogador; 'J' jogar de novo (mesmos jogadores);
        'F' encerrar a vez agora; 'X' sair da rodada; 'C' recomeçar a música (a vez também);
        'Z' pular para a próxima música; 'R' resultado da vez + ranking da rodada + recordes;
        'K' recordes da música que está tocando."""
        c = chr(cmd)
        with self.trava:
            s, m = self.sessao, self.musica
            ident = self.id_musica(m[0], m[1]) if m and m[0] else 0
            if c == "L":
                return self._lista()
            if c == "N" and dados:
                self.perfis.criar(dados[1:].decode("utf-8", "replace"), dados[0])
                return self._lista()
            if c == "A" and dados:
                self.perfis.apagar(dados[0])
                return self._lista()
            if c == "S" and dados:
                jogadores = []
                for pid in dados[1:1 + min(dados[0], MAX_JOGADORES)]:
                    p = self.perfis.achar(pid) if pid else None
                    jogadores.append((pid, p["nome"], p["cor"]) if p else (0, "Convidado", 7))
                if not jogadores:
                    jogadores = [(0, "Convidado", 7)]
                n = min(dados[0], MAX_JOGADORES)
                dificuldade = dados[1 + n] if len(dados) > 1 + n and dados[1 + n] <= DIFICIL else NORMAL
                self.sessao = Sessao(jogadores, (ident, m[0], m[1]) if ident else None, dificuldade)
                if ident:
                    self.comandos.put("reiniciar")
                print("Karaokê: rodada com", ", ".join(j[1] for j in jogadores))
                return "ok"
            if c == "P" and s and s.estado == Sessao.FIM_DA_VEZ:
                s.proxima()
                self._voltar_musica()
                return "ok"
            if c == "J" and s and s.estado == Sessao.FIM:
                self.sessao = Sessao(s.jogadores, s.musica, s.dif)
                self._voltar_musica()
                return "ok"
            if c == "F" and s and s.estado == Sessao.CANTANDO:
                self._fim_da_vez()
                self.comandos.put("pausar")
                return "ok"
            if c == "X":
                self.sessao = None
                return "ok"
            if c == "C":
                if s and s.estado == Sessao.CANTANDO:
                    s.vezes[-1] = Vez(*s.jogadores[len(s.vezes) - 1], s.dif)
                    s.desde = time.monotonic()
                self.comandos.put("reiniciar")
                return "ok"
            if c == "Z":
                self.comandos.put("proxima")
                return "ok"
            if c == "R" and s and s.vez.resultado:
                r, info = s.vez.resultado, s.vez.info
                k = r["contagem"]
                linhas = [f"{r['pontos']}\t{r['precisao']}\t{r['estrelas']}\t{r['combo_max']}\t{k[PERFEITO]}\t"
                          f"{k[OTIMO]}\t{k[BOM]}\t{k[ERROU]}\t{int(info.get('recorde', False))}\t"
                          f"{int(info.get('recorde_geral', False))}\t{info.get('nivel', 0)}\t"
                          f"{int(info.get('subiu', False))}\t{info.get('xp', 0)}\t{int(bool(s.vez.pid))}\n"]
                feitas = sorted((v for v in s.vezes if v.resultado), key=lambda v: -v.resultado["pontos"])
                linhas += [f"J\t{texto(v.nome, 16)}\t{v.cor}\t{v.resultado['pontos']}\t{v.resultado['estrelas']}\n"
                           for v in feitas]
                if len(s.vezes) < len(s.jogadores):            # quem canta depois
                    linhas.append(f"N\t{texto(s.jogadores[len(s.vezes)][1], 16)}\n")
                return "".join(linhas) + self._tops(s.musica[0] if s.musica else ident)
            if c == "K":
                return self._tops(ident)
        return ""

    # ------------------------------------------------ rede
    def _loop(self):
        while True:
            try:
                dados, origem = self.sock.recvfrom(512)
            except (socket.timeout, OSError):
                self.status["conectado"] = time.monotonic() - self.ultimo_3ds < 5
                continue
            try:
                self._pacote(dados, origem)
            except Exception as e:                     # um pacote estranho não derruba o karaokê
                print("Karaokê: erro", repr(e))

    def _pacote(self, dados, origem):
        if len(dados) >= 7 and dados[:4] == b"3DSK":
            if time.monotonic() - self.ultimo_3ds > 5:
                print(f"Karaokê: 3DS conectado ({origem[0]})")
            self.ultimo_3ds = time.monotonic()
            self.ouvido.ativo_ate = self.ultimo_3ds + 5
            self.status["conectado"] = True
            freq, volume = struct.unpack("<HB", dados[4:7])
            flags = dados[7] if len(dados) >= 8 else 0
            m = self.musica
            pos = self._posicao(m) if m else 0
            ident = self.id_musica(m[0], m[1]) if m and m[0] else 0
            if flags & 2:                              # 3DS novo: o jogo
                alvo, acerto = self._jogar(freq, volume, flags, m, pos, ident)
                self.sock.sendto(self._estado_jogo(m, pos, ident, alvo, acerto), origem)
                return
            alvo = self._nota_da_voz(freq, volume)
            if m:
                titulo, artista, tocando = m[0], m[1], m[2]
                letra = self.letras.get(ident, None)
                estado_letra = 0 if letra is None else (1 if letra else 2)
                nome = f"{texto(titulo, 50)}\n{texto(artista, 40)}"
            else:
                tocando, estado_letra, nome = False, 2, ""
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
        elif len(dados) >= 6 and dados[:4] == b"3DSP":
            seq, cmd = dados[4], dados[5]
            resposta = self._comando(cmd, dados[6:]).encode("utf-8")[:1100]
            self.sock.sendto(b"3DSP" + bytes([seq, cmd]) + resposta, origem)
