"""Steam: a biblioteca da Steam do PC no 3DS, e o jogo na tela de cima.

Lê os jogos instalados direto dos arquivos da Steam (sem login), manda as capas,
abre o jogo pelo link steam:// e depois manda a imagem da janela do jogo, como o
Espelho. Os botões vão pelo Controle (UDP 5005), como um controle de Xbox.

A imagem do jogo é copiada sem parar numa thread (Captura) e o último quadro fica
pronto: o 3DS recebe na hora, sem esperar a cópia. Com o jogo na frente, a cópia usa
o Desktop Duplication (dxcam, o mesmo do OBS), bem mais rápido que o GDI.

Protocolo (TCP 5012, o 3DS pede e o PC responde):
    "J\\n"           -> "3DSJ" + u32 tamanho + linhas "appid\\tnome\\tminutos jogados\\n"
                        (do jogado mais recente para o mais antigo)
    "C appid w h\\n" -> "3DSC" + u32 tamanho + JPEG w x h da capa (tamanho 0 = sem capa)
    "P appid\\n"     -> abre o jogo: "3DSO" + u32 tamanho + texto ("" = ok, senão o erro)
    "F w h\\n"       -> "3DSG" + u32 tamanho + u8 estado + JPEG w x h da janela do jogo
                        estado: 0 = esperando o jogo abrir, 1 = imagem, 2 = o jogo fechou
    "T 400 240 k\\n" -> modo Blocos: "3DST" + u32 tamanho + u8 estado + u8 n + n números
                        de bloco (0 a 24, linha por linha) + JPEG do mosaico com esses
                        blocos, 5 por linha (k = 1: manda a tela inteira)
    "Q\\n"           -> fecha o jogo: pede para as janelas dele fecharem e, depois de 6 s,
                        encerra à força o que sobrou da pasta do jogo (sem resposta)
"""

import ctypes
import glob
import io
import os
import re
import socket
import struct
import threading
import time
import urllib.request
import winreg

import psutil
import win32api
import win32con
import win32gui
import win32process
from PIL import Image, ImageDraw, ImageGrab

from espelho import capturar, texto
from util import SSL, trazer_para_frente

try:
    import dxcam                 # captura rápida (Desktop Duplication)
    import numpy as np
except Exception:                # sem a biblioteca: usa a cópia comum (mais lenta)
    dxcam = None

PORTA = 5012
QUADROS_POR_SEGUNDO = 30         # no máximo; o 3DS pega o último pronto
PASTA = os.path.dirname(os.path.abspath(__file__))
PASTA_CAPAS = os.path.join(PASTA, "capas")       # capas baixadas da internet
MAX_JOGOS = 200

# coisas que aparecem na biblioteca mas não são jogos
NAO_JOGOS = re.compile(r"redistributable|steamworks|proton|steam linux runtime|steamvr|"
                       r"dedicated server|soundtrack|\bsdk\b", re.I)
PROCESSOS_STEAM = {"steam.exe", "steamwebhelper.exe", "steamservice.exe", "explorer.exe",
                   "gameoverlayui.exe", "pythonw.exe", "python.exe", "py.exe"}
# janelas do próprio Windows que aparecem na frente sozinhas (busca, menu Iniciar...)
PROCESSOS_SISTEMA = {"searchhost.exe", "searchapp.exe", "startmenuexperiencehost.exe",
                     "shellexperiencehost.exe", "applicationframehost.exe", "textinputhost.exe",
                     "systemsettings.exe", "lockapp.exe", "securityhealthsystray.exe",
                     "widgets.exe", "phoneexperiencehost.exe"}


# ---------------------------------------------------------------- arquivos da Steam
def ler_vdf(caminho):
    """Lê um arquivo de texto da Steam (VDF/ACF) como dicionários. Chaves em minúsculas."""
    with open(caminho, encoding="utf-8", errors="replace") as f:
        conteudo = f.read()
    raiz, pilha, chave = {}, [], None
    atual = raiz
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"|([{}])', conteudo):
        if m.group(2) == "{":
            novo = {}
            atual[(chave or "").lower()] = novo
            pilha.append(atual)
            atual, chave = novo, None
        elif m.group(2) == "}":
            atual = pilha.pop() if pilha else raiz
            chave = None
        else:
            valor = m.group(1).replace("\\\\", "\\")
            if chave is None:
                chave = valor
            else:
                atual[chave.lower()] = valor
                chave = None
    return raiz


def pasta_steam():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            caminho = winreg.QueryValueEx(k, "SteamPath")[0]
            if os.path.isdir(caminho):
                return os.path.normpath(caminho)
    except OSError:
        pass
    for caminho in (r"C:\Program Files (x86)\Steam", r"C:\Program Files\Steam"):
        if os.path.isdir(caminho):
            return caminho
    return None


def tempo_jogado(steam):
    """Minutos jogados por appid, do usuário da Steam que jogou por último."""
    arquivos = glob.glob(os.path.join(steam, "userdata", "*", "config", "localconfig.vdf"))
    if not arquivos:
        return {}
    try:
        cfg = ler_vdf(max(arquivos, key=os.path.getmtime))
        apps = cfg["userlocalconfigstore"]["software"]["valve"]["steam"]["apps"]
    except (OSError, KeyError, TypeError):
        return {}
    saida = {}
    for appid, dados in apps.items():
        if isinstance(dados, dict):
            try:
                saida[appid] = int(dados.get("playtime", 0))
            except ValueError:
                pass
    return saida


def listar_jogos():
    """Jogos instalados: [{appid, nome, pasta, ultimo, minutos}], o jogado mais recente primeiro."""
    steam = pasta_steam()
    if not steam:
        return []
    bibliotecas = [steam]
    try:
        vdf = ler_vdf(os.path.join(steam, "steamapps", "libraryfolders.vdf"))
        for item in vdf.get("libraryfolders", {}).values():
            if isinstance(item, dict) and item.get("path"):
                bibliotecas.append(item["path"])
    except OSError:
        pass

    minutos = tempo_jogado(steam)
    jogos, vistos = [], set()
    for bib in bibliotecas:
        apps = os.path.join(bib, "steamapps")
        for acf in glob.glob(os.path.join(apps, "appmanifest_*.acf")):
            try:
                app = ler_vdf(acf).get("appstate", {})
            except OSError:
                continue
            appid, nome = app.get("appid", ""), app.get("name", "")
            if not appid or appid in vistos or not nome or NAO_JOGOS.search(nome):
                continue
            vistos.add(appid)
            jogos.append({
                "appid": appid,
                "nome": nome,
                "pasta": os.path.normcase(os.path.join(apps, "common", app.get("installdir", ""))),
                "ultimo": int(app.get("lastplayed", 0) or 0),
                "minutos": minutos.get(appid, 0),
            })
    jogos.sort(key=lambda j: (-j["ultimo"], j["nome"].lower()))
    return jogos[:MAX_JOGOS]


# ---------------------------------------------------------------- capas
def arquivo_capa(appid):
    """A capa larga (460x215) do jogo: do cache da Steam ou, se não tiver, da internet."""
    steam = pasta_steam()
    if steam:
        cache = os.path.join(steam, "appcache", "librarycache")
        for padrao in (f"{appid}/header.jpg", f"{appid}/*/header.jpg", f"{appid}_header.jpg",
                       f"{appid}/library_header.jpg", f"{appid}/*/library_header.jpg"):
            achados = glob.glob(os.path.join(cache, padrao))
            if achados:
                return achados[0]

    os.makedirs(PASTA_CAPAS, exist_ok=True)
    local = os.path.join(PASTA_CAPAS, f"{appid}.jpg")
    if os.path.exists(local):
        return local if os.path.getsize(local) else None
    for url in (f"https://cdn.cloudflare.steamstatic.com/steam/apps/{appid}/header.jpg",
                f"https://shared.cloudflare.steamstatic.com/store_item_assets/steam/apps/{appid}/header.jpg"):
        try:
            with urllib.request.urlopen(url, timeout=6, context=SSL) as r:
                dados = r.read()
            with open(local, "wb") as f:
                f.write(dados)
            return local
        except Exception:
            continue
    open(local, "wb").close()          # não tem: não tenta baixar de novo toda hora
    return None


def capa_sem_imagem(nome, w, h):
    img = Image.new("RGB", (w, h), (32, 37, 58))
    d = ImageDraw.Draw(img)
    d.text((6, h // 2 - 6), nome[:max(4, w // 7)], fill=(232, 234, 242))
    return img


def capa_jpeg(jogo, w, h):
    img = None
    caminho = arquivo_capa(jogo["appid"])
    if caminho:
        try:
            img = Image.open(caminho).convert("RGB")
        except Exception:
            img = None
    if img is None:
        img = capa_sem_imagem(jogo["nome"], w, h)
    # preenche w x h cortando as sobras (as capas são quase todas 460x215)
    escala = max(w / img.width, h / img.height)
    img = img.resize((max(w, round(img.width * escala)), max(h, round(img.height * escala))), Image.BILINEAR)
    x0, y0 = (img.width - w) // 2, (img.height - h) // 2
    img = img.crop((x0, y0, x0 + w, y0 + h))
    saida = io.BytesIO()
    img.save(saida, "JPEG", quality=80)
    return saida.getvalue()


# ---------------------------------------------------------------- janela do jogo
def exe_da_janela(hwnd):
    try:
        _, pid = win32process.GetWindowThreadProcessId(hwnd)
        return os.path.normcase(psutil.Process(pid).exe())
    except Exception:
        return ""


def janelas_visiveis():
    saida = []

    def cb(hwnd, _):
        if not win32gui.IsWindowVisible(hwnd) or not win32gui.GetWindowText(hwnd):
            return True
        if win32gui.GetWindowLong(hwnd, win32con.GWL_EXSTYLE) & win32con.WS_EX_TOOLWINDOW:
            return True
        x0, y0, x1, y1 = win32gui.GetWindowRect(hwnd)
        if x1 - x0 >= 200 and y1 - y0 >= 150:
            saida.append((hwnd, (x1 - x0) * (y1 - y0)))
        return True

    win32gui.EnumWindows(cb, None)
    return saida


def janela_da_pasta(jogo):
    """A maior janela de um programa que está dentro da pasta do jogo."""
    melhor, area_melhor = None, 0
    for hwnd, area in janelas_visiveis():
        if exe_da_janela(hwnd).startswith(jogo["pasta"] + os.sep) and area > area_melhor:
            melhor, area_melhor = hwnd, area
    return melhor


def achar_janela(jogo, antes, desde, aberto_em):
    """(janela, é da pasta do jogo). Primeiro, uma janela de programa da pasta do jogo.
    Se o jogo abre por um launcher de fora da pasta, depois de alguns segundos vale a
    janela da frente, mas só de um programa que abriu depois do pedido (assim a busca
    do Windows, o Steam ou o navegador não viram "o jogo")."""
    melhor = janela_da_pasta(jogo)
    if melhor:
        return melhor, True
    if time.monotonic() - desde > 6:
        frente = win32gui.GetForegroundWindow()
        if frente and frente not in antes and win32gui.IsWindowVisible(frente):
            try:
                _, pid = win32process.GetWindowThreadProcessId(frente)
                processo = psutil.Process(pid)
                nome = processo.name().lower()
                novo = processo.create_time() >= aberto_em - 2
            except Exception:
                return None, False
            if novo and nome not in PROCESSOS_STEAM and nome not in PROCESSOS_SISTEMA:
                return frente, False
    return None, False


def processos_do_jogo(jogo, janela=None):
    """Os programas do jogo: tudo que roda da pasta dele (e o que esses abriram), mais o
    dono da janela capturada (jogos que abrem por um launcher de fora da pasta)."""
    pasta = jogo["pasta"] + os.sep
    achados = {}
    for p in psutil.process_iter(["exe"]):
        exe = p.info.get("exe") or ""
        if exe and os.path.normcase(exe).startswith(pasta):
            achados[p.pid] = p
    if janela and win32gui.IsWindow(janela):
        try:
            _, pid = win32process.GetWindowThreadProcessId(janela)
            p = psutil.Process(pid)
            if p.name().lower() not in PROCESSOS_STEAM | PROCESSOS_SISTEMA:
                achados[pid] = p
        except Exception:
            pass
    for p in list(achados.values()):
        try:
            for filho in p.children(recursive=True):
                if filho.name().lower() not in PROCESSOS_STEAM | PROCESSOS_SISTEMA:
                    achados[filho.pid] = filho
        except Exception:
            pass
    return list(achados.values())


def fechar_jogo(jogo, janela=None, espera=6):
    """Fecha o jogo no PC: pede para as janelas dele fecharem (o jogo pode salvar) e,
    passados alguns segundos, encerra à força o que ainda estiver aberto."""
    processos = processos_do_jogo(jogo, janela)
    if not processos:
        print(f"Steam: {jogo['nome']} já estava fechado")
        return
    pids = {p.pid for p in processos}

    def pedir(hwnd, _):
        try:
            if win32process.GetWindowThreadProcessId(hwnd)[1] in pids:
                win32api.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
        except Exception:
            pass
        return True

    win32gui.EnumWindows(pedir, None)
    _, vivos = psutil.wait_procs(processos, timeout=espera)
    for p in vivos:
        try:
            p.kill()
        except psutil.Error:
            pass
    psutil.wait_procs(vivos, timeout=3)
    print(f"Steam: {jogo['nome']} fechado" + (f" ({len(vivos)} à força)" if vivos else ""))


class TelaRapida:
    """Cópia da tela pelo Desktop Duplication: uns 10 ms em 1080p, contra 50 a 90 ms
    do GDI. Só no monitor principal; fora dele (ou sem o dxcam) usa a cópia comum."""

    def __init__(self):
        self.cam = dxcam.create(output_idx=0, output_color="BGRA")
        self.w, self.h = self.cam.width, self.cam.height
        self.caixa = None                # o que o último quadro mostrou

    def copiar(self, caixa, largura_alvo):
        """(True, imagem), (True, None) = a tela não mudou, (False, None) = não deu."""
        l, t, r, b = caixa
        if l < 0 or t < 0 or r > self.w or b > self.h:
            return False, None
        mesma = caixa == self.caixa
        a = self.cam.grab(region=caixa, new_frame_only=mesma)
        if a is None:
            return mesma, None
        self.caixa = caixa
        # pula pixels já aqui (fica com uns 2x o tamanho final): converter 1080p
        # inteiro para o Pillow custaria mais que a própria cópia
        passo = max(1, int((r - l) / (2 * largura_alvo)))
        if passo > 1:
            a = a[::passo, ::passo]
        a = np.ascontiguousarray(a)
        return True, Image.frombuffer("RGB", (a.shape[1], a.shape[0]), a.tobytes(), "raw", "BGRX", 0, 1)


def capturar_jogo(hwnd, tela=None, largura_alvo=400):
    """Só a parte de dentro da janela (sem a barra de título). None = nada novo.
    Com o jogo na frente, copia da tela (funciona com jogos em DirectX); atrás de
    outra janela, usa o PrintWindow e, se o jogo não deixar, espera ele voltar para
    a frente (copiar a tela ali mostraria a janela que está por cima)."""
    if win32gui.IsIconic(hwnd):
        return None
    esq, topo = win32gui.ClientToScreen(hwnd, (0, 0))
    _, _, w, h = win32gui.GetClientRect(hwnd)
    if w < 16 or h < 16:
        return None
    caixa = (esq, topo, esq + w, topo + h)
    if win32gui.GetForegroundWindow() == hwnd:
        if tela:
            ok, img = tela.copiar(caixa, largura_alvo)
            if ok:
                return img
        return ImageGrab.grab(bbox=caixa, all_screens=True)
    img = capturar(hwnd, so_janela=True)
    if img is None:
        return None
    x0, y0, _, _ = win32gui.GetWindowRect(hwnd)
    # Jogo que não conhece a escala do Windows (ex.: Balatro com a tela em 125%): ele
    # desenha menor e o Windows estica só na tela. A cópia da janela vem sem esticar,
    # com o jogo no canto: recorta só a parte desenhada (o tamanho volta ao normal
    # quando a imagem é ajustada para o 3DS).
    f = escala_desenho(hwnd)
    return img.crop((round((esq - x0) * f), round((topo - y0) * f),
                     round((esq - x0 + w) * f), round((topo - y0 + h) * f)))


def escala_desenho(hwnd):
    """Quanto do tamanho real a janela desenha: 1.0 normalmente; 0.8 num jogo que
    ignora a escala de 125% do Windows."""
    try:
        user32 = ctypes.windll.user32
        janela = user32.GetDpiForWindow(hwnd)
        monitor = ctypes.c_uint(0)
        ctypes.windll.shcore.GetDpiForMonitor(user32.MonitorFromWindow(hwnd, 2), 0,
                                              ctypes.byref(monitor), ctypes.byref(ctypes.c_uint(0)))
        if janela and monitor.value and janela < monitor.value:
            return janela / monitor.value
    except Exception:
        pass
    return 1.0


class Captura:
    """Copia a janela do jogo sem parar (até QUADROS_POR_SEGUNDO) e guarda o último
    quadro já encaixado no tamanho do 3DS (a compressão fica com quem pede: o JPEG
    inteiro ou só os blocos que mudaram). Só trabalha enquanto o 3DS pede quadros."""

    def __init__(self, config):
        self.config = config
        self.hwnd, self.tam = None, (400, 240)
        self.quadro = (0, None, None)    # (número, (w, h), imagem w x h)
        self.pedido_em = 0.0
        self.ativa = True
        self.cond = threading.Condition()
        threading.Thread(target=self._loop, daemon=True, name="steam-captura").start()

    def parar(self):
        self.ativa = False

    def _loop(self):
        tela = None
        if dxcam:
            try:
                tela = TelaRapida()
            except Exception as e:
                print("Steam: captura rápida indisponível, usando a comum:", repr(e))
        while self.ativa:
            hwnd, (w, h) = self.hwnd, self.tam
            if not hwnd or time.monotonic() - self.pedido_em > 2:   # 3DS na biblioteca
                time.sleep(0.05)
                continue
            t0 = time.monotonic()
            try:
                img = capturar_jogo(hwnd, tela, w)
                if img is not None:
                    tela_3ds = encaixar(img, w, h)
                    with self.cond:
                        self.quadro = (self.quadro[0] + 1, (w, h), tela_3ds)
                        self.cond.notify_all()
            except Exception as e:
                print("Steam: erro ao capturar", repr(e))
                time.sleep(0.2)
            time.sleep(max(0.003, 1 / QUADROS_POR_SEGUNDO - (time.monotonic() - t0)))

    def proximo(self, hwnd, depois_de, w, h, espera=0.25):
        """Um quadro mais novo que 'depois_de' no tamanho pedido. Se a imagem não mudou
        nesse tempo, repete o último (o 3DS continua o ciclo). (número, imagem ou None)."""
        self.hwnd, self.tam, self.pedido_em = hwnd, (w, h), time.monotonic()
        fim = time.monotonic() + espera
        with self.cond:
            while True:
                n, tam, img = self.quadro
                if n > depois_de and tam == (w, h):
                    return n, img
                resto = fim - time.monotonic()
                if resto <= 0:
                    return (n, img) if tam == (w, h) else (depois_de, None)
                self.cond.wait(resto)


def encaixar(img, w, h):
    """Encaixa a imagem em w x h com faixas pretas (não corta o HUD do jogo)."""
    escala = min(w / img.width, h / img.height)
    nw, nh = max(1, round(img.width * escala)), max(1, round(img.height * escala))
    # reducing_gap: reduz em passos inteiros antes (bem mais rápido em 1080p)
    img = img.resize((nw, nh), Image.BILINEAR, reducing_gap=2.0)
    tela = Image.new("RGB", (w, h))
    tela.paste(img, ((w - nw) // 2, (h - nh) // 2))
    return tela


def para_jpeg(img, qualidade):
    saida = io.BytesIO()
    img.save(saida, "JPEG", quality=qualidade)
    return saida.getvalue()


def quadro_jpeg(img, w, h, qualidade):
    return para_jpeg(encaixar(img, w, h), qualidade)


# ---------------------------------------------------------------- modo Blocos
# A tela do 3DS (400x240) vira 5 x 5 blocos de 80x48. Só os blocos que mudaram
# desde o último envio vão, juntos numa imagem só ("mosaico", 5 por linha), mais a
# lista de onde cada um fica. Os tamanhos são múltiplos de 16: os quadradinhos do
# JPEG não misturam um bloco com o vizinho do mosaico.
BLOCO_W, BLOCO_H = 80, 48
BLOCOS_X, BLOCOS_Y = 400 // BLOCO_W, 240 // BLOCO_H
N_BLOCOS = BLOCOS_X * BLOCOS_Y


class Blocos:
    """Lembra o que o 3DS já tem na tela e monta só a diferença."""

    def __init__(self):
        self.tem = None                  # o que o 3DS mostra (numpy 240x400x3)
        self.proximo_refresco = 0        # um bloco por vez é reenviado mesmo sem mudar
        self.refresco_em = 0.0

    def montar(self, img, limiar, qualidade, tudo=False):
        """(lista de blocos, JPEG do mosaico). Lista vazia = nada mudou."""
        atual = np.asarray(img, dtype=np.int16)
        if tudo or self.tem is None:
            mudou = list(range(N_BLOCOS))
        else:
            dif = np.abs(atual - self.tem).reshape(BLOCOS_Y, BLOCO_H, BLOCOS_X, BLOCO_W, 3).mean(axis=(1, 3, 4))
            mudou = [int(i) for i in np.flatnonzero(dif.ravel() > limiar)]
            # de vez em quando reenvia um bloco parado: corrige o que o JPEG ou um
            # quadro perdido tiver deixado diferente (a tela toda a cada ~5 s)
            if time.monotonic() >= self.refresco_em:
                self.refresco_em = time.monotonic() + 0.2
                if self.proximo_refresco not in mudou:
                    mudou.append(self.proximo_refresco)
                self.proximo_refresco = (self.proximo_refresco + 1) % N_BLOCOS
        if not mudou:
            return [], b""
        colunas = min(BLOCOS_X, len(mudou))
        linhas = (len(mudou) + BLOCOS_X - 1) // BLOCOS_X
        mosaico = Image.new("RGB", (colunas * BLOCO_W, linhas * BLOCO_H))
        if self.tem is None:
            self.tem = atual.copy()
        for k, b in enumerate(mudou):
            x, y = (b % BLOCOS_X) * BLOCO_W, (b // BLOCOS_X) * BLOCO_H
            caixa = (x, y, x + BLOCO_W, y + BLOCO_H)
            mosaico.paste(img.crop(caixa), ((k % BLOCOS_X) * BLOCO_W, (k // BLOCOS_X) * BLOCO_H))
            self.tem[y:y + BLOCO_H, x:x + BLOCO_W] = atual[y:y + BLOCO_H, x:x + BLOCO_W]
        return mudou, para_jpeg(mosaico, qualidade)


# ---------------------------------------------------------------- servidor
class Steam:
    def __init__(self, config):
        self.config = config
        self.status = {"conectado": False, "jogo": ""}

    def iniciar(self):
        self.servidor = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.servidor.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        self.servidor.bind(("0.0.0.0", PORTA))
        self.servidor.listen(1)
        threading.Thread(target=self._aceitar, daemon=True, name="steam").start()

    def _aceitar(self):
        while True:
            try:
                conn, origem = self.servidor.accept()
            except OSError:
                time.sleep(1)
                continue
            print(f"Steam: 3DS conectado ({origem[0]})")
            self.status["conectado"] = True
            try:
                self._atender(conn)
            except Exception as e:
                print("Steam: desconectou", repr(e))
            finally:
                conn.close()
                self.status.update(conectado=False, jogo="")

    def _atender(self, conn):
        conn.settimeout(60)
        arquivo = conn.makefile("rb")
        captura = Captura(self.config)
        try:
            self._comandos(conn, arquivo, captura)
        finally:
            captura.parar()

    def _comandos(self, conn, arquivo, captura):
        jogos = {}
        jogo = janela = None               # jogo aberto pelo 3DS e a janela dele
        antes, desde, achou_uma_vez, sumiu_em = set(), 0.0, False, None
        aberto_em, da_pasta, conferiu_em = 0.0, False, 0.0
        enviado = 0                         # número do último quadro mandado
        pediu_fechar = False                # o 3DS mandou fechar: avisa assim que a janela sumir
        blocos = Blocos()

        def responder(magica, dados, extra=b""):
            conn.sendall(magica + struct.pack("<I", len(dados)) + extra + dados)

        while True:
            linha = arquivo.readline()
            if not linha:
                return
            partes = linha.decode("ascii", "ignore").split()
            if not partes:
                continue
            cmd = partes[0]

            if cmd == "J":
                lista = listar_jogos()
                jogos = {j["appid"]: j for j in lista}
                corpo = "".join(f"{j['appid']}\t{texto(j['nome'], 40)}\t{j['minutos']}\n" for j in lista)
                responder(b"3DSJ", corpo.encode("utf-8"))

            elif cmd == "C" and len(partes) == 4:
                dados = b""
                try:
                    w, h = max(8, min(400, int(partes[2]))), max(8, min(240, int(partes[3])))
                    if partes[1] in jogos:
                        dados = capa_jpeg(jogos[partes[1]], w, h)
                except Exception as e:
                    print("Steam: erro na capa", repr(e))
                responder(b"3DSC", dados)

            elif cmd == "P" and len(partes) == 2:
                erro = ""
                jogo = jogos.get(partes[1])
                if not jogo:
                    erro = "Jogo não encontrado no PC."
                else:
                    antes = {h for h, _ in janelas_visiveis()}
                    desde, aberto_em = time.monotonic(), time.time()
                    janela, achou_uma_vez, sumiu_em, enviado = None, False, None, 0
                    pediu_fechar = False
                    # já aberto? usa a janela que existe (não abre outro)
                    janela = janela_da_pasta(jogo)
                    da_pasta = janela is not None
                    if janela:
                        trazer_para_frente(janela)
                        achou_uma_vez = True
                    else:
                        try:
                            os.startfile(f"steam://rungameid/{jogo['appid']}")
                        except OSError as e:
                            erro = f"Não consegui abrir: {e}"
                    self.status["jogo"] = jogo["nome"]
                    print(f"Steam: abrindo {jogo['nome']} ({jogo['appid']})")
                responder(b"3DSO", erro.encode("utf-8"))

            elif (cmd == "F" and len(partes) == 3) or (cmd == "T" and len(partes) == 4):
                estado, dados, img = 0, b"", None
                try:
                    w, h = max(16, min(400, int(partes[1]))), max(16, min(240, int(partes[2])))
                except ValueError:
                    w, h = 400, 240
                if cmd == "T":                  # blocos: sempre a tela inteira do 3DS
                    w, h = 400, 240
                if jogo:
                    if janela and not win32gui.IsWindow(janela):
                        janela = None
                    # janela achada pela regra reserva: quando a do jogo aparecer, troca
                    if janela and not da_pasta and time.monotonic() - conferiu_em > 2:
                        conferiu_em = time.monotonic()
                        if janela_da_pasta(jogo):
                            janela = None
                    if not janela:
                        janela, da_pasta = achar_janela(jogo, antes, desde, aberto_em)
                        if janela:
                            achou_uma_vez = True
                            print(f"Steam: janela do jogo: {win32gui.GetWindowText(janela)!r}")
                            trazer_para_frente(janela)   # para o controle ir para o jogo
                    if janela:
                        sumiu_em = None
                        enviado, img = captura.proximo(janela, enviado, w, h)
                        estado = 1 if img is not None else 0
                    elif achou_uma_vez:
                        # a janela sumiu: pode ser só a tela de abertura trocando pela do
                        # jogo. Só diz que fechou depois de alguns segundos sem janela.
                        sumiu_em = sumiu_em or time.monotonic()
                        if pediu_fechar or time.monotonic() - sumiu_em > 8:
                            estado = 2
                    elif pediu_fechar:
                        estado = 2
                cfg = self.config["steam"]
                if cmd == "F":
                    if img is not None:
                        dados = para_jpeg(img, cfg["qualidade_jpeg"])
                    responder(b"3DSG", dados, bytes([estado]))
                else:
                    # "T 400 240 k": só os blocos que mudaram (k = 1: a tela inteira)
                    lista = []
                    if img is not None:
                        lista, dados = blocos.montar(img, cfg["limiar_blocos"], cfg["qualidade_jpeg"],
                                                     tudo=partes[3] == "1")
                    responder(b"3DST", dados, bytes([estado, len(lista)] + lista))

            elif cmd == "Q":
                # sem resposta (o 3DS não espera): fecha num canto, sem segurar os quadros
                if jogo:
                    pediu_fechar = True
                    threading.Thread(target=fechar_jogo, args=(jogo, janela), daemon=True,
                                     name="steam-fechar").start()
