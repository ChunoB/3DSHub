"""Espelho: manda a imagem de uma janela do PC (ou da tela inteira de um monitor) para
a tela de cima do 3DS. A lista começa com "Tela inteira" (uma por monitor).

Protocolo (TCP, o 3DS pede e o PC responde):
    "L\\n"      -> "3DSL" + u32 tamanho + títulos separados por \\n
    "S <n>\\n"  -> escolhe a janela n da última lista
    "F\\n"      -> "3DSF" + u32 tamanho + JPEG 400x240 (tamanho 0 = sem janela)
    "F z x y\\n" -> com zoom (z = zoom x100, x/y = centro x10000):
                  "3DSZ" + u32 tamanho + u16 x + u16 y (centro efetivo) + JPEG
    "T u v b\\n" -> clique no ponto (u, v de 0 a 10000 na imagem; b 1 = esquerdo, 2 = direito)
    "M u v\\n"   -> só leva o mouse até o ponto (sem clicar)
    "R u v n\\n" -> rodinha do mouse no ponto (n passos; positivo = sobe)
    "K <hex>\\n" -> digita o texto (UTF-8 em hex) na janela
    "E <tecla>\\n" -> aperta uma tecla: enter, backspace, tab ou esc
T, M, R, K e E não têm resposta. Os títulos vão em UTF-8 (com acentos).

O cursor do mouse aparece desenhado na imagem quando está em cima da janela. O
clique deixa o mouse no ponto clicado (antes ele voltava para onde estava), então
dá para ver onde clicou e dois toques rápidos viram clique duplo no PC.
"""

import ctypes
import io
import socket
import struct
import threading
import time
import unicodedata

import win32api
import win32con
import win32gui
import win32ui
from PIL import Image, ImageDraw, ImageGrab

from util import Button, Key, mouse, teclado, trazer_para_frente

PORTA = 5008
LARGURA, ALTURA = 400, 240
MAX_JANELAS = 12

user32 = ctypes.windll.user32
try:
    user32.SetProcessDPIAware()   # coordenadas reais em telas com zoom
except Exception:
    pass


def texto(t, maximo=38):
    """Título para o 3DS: mantém os acentos (a interface nova mostra), tira
    quebras de linha e caracteres de controle, e corta por letras (não bytes)."""
    t = unicodedata.normalize("NFC", t or "")
    t = "".join(c for c in t if c.isprintable())
    return t.strip()[:maximo]


class Tela:
    """Um monitor inteiro (em vez de uma janela). rect = (x0, y0, x1, y1) na área de trabalho."""

    def __init__(self, rect, titulo):
        self.rect, self.titulo = rect, titulo


def listar_telas():
    try:
        monitores = [m[2] for m in win32api.EnumDisplayMonitors()]
    except Exception:
        return []
    monitores.sort(key=lambda r: (r[0], r[1]))
    saida = []
    for i, (x0, y0, x1, y1) in enumerate(monitores):
        nome = "Tela inteira" if len(monitores) == 1 else f"Tela inteira {i + 1}"
        saida.append((Tela((x0, y0, x1, y1), nome), f"{nome} ({x1 - x0}x{y1 - y0})"))
    return saida


# o que o Espelho mostra pode ser uma janela (hwnd) ou uma Tela
def existe(alvo):
    return isinstance(alvo, Tela) or bool(alvo and win32gui.IsWindow(alvo))


def retangulo(alvo):
    return alvo.rect if isinstance(alvo, Tela) else win32gui.GetWindowRect(alvo)


def capturar_alvo(alvo):
    if isinstance(alvo, Tela):
        return ImageGrab.grab(bbox=alvo.rect, all_screens=True)
    return capturar(alvo)


def titulo_de(alvo):
    return alvo.titulo if isinstance(alvo, Tela) else win32gui.GetWindowText(alvo)


def listar_janelas():
    janelas = []

    def cb(hwnd, _):
        if not win32gui.IsWindowVisible(hwnd) or win32gui.IsIconic(hwnd):
            return True
        titulo = win32gui.GetWindowText(hwnd)
        if not titulo or titulo in ("Program Manager",):
            return True
        if win32gui.GetWindowLong(hwnd, win32con.GWL_EXSTYLE) & win32con.WS_EX_TOOLWINDOW:
            return True
        x0, y0, x1, y1 = win32gui.GetWindowRect(hwnd)
        if x1 - x0 < 150 or y1 - y0 < 100:
            return True
        janelas.append((hwnd, titulo))
        return True

    win32gui.EnumWindows(cb, None)
    telas = listar_telas()
    return telas + janelas[:MAX_JANELAS - len(telas)]


def capturar(hwnd, so_janela=False):
    """Captura a janela mesmo se estiver atrás de outras (PrintWindow).
    so_janela: se a janela não aceitar, volta None em vez de copiar da tela
    (que mostraria o que estiver por cima dela)."""
    x0, y0, x1, y1 = win32gui.GetWindowRect(hwnd)
    w, h = x1 - x0, y1 - y0
    if w <= 0 or h <= 0:
        return None
    dc_janela = win32gui.GetWindowDC(hwnd)
    dc = dc_mem = bmp = None
    try:
        dc = win32ui.CreateDCFromHandle(dc_janela)
        dc_mem = dc.CreateCompatibleDC()
        bmp = win32ui.CreateBitmap()
        bmp.CreateCompatibleBitmap(dc, w, h)
        dc_mem.SelectObject(bmp)
        ok = user32.PrintWindow(hwnd, dc_mem.GetSafeHdc(), 2)   # 2 = PW_RENDERFULLCONTENT
        info = bmp.GetInfo()
        img = Image.frombuffer("RGB", (info["bmWidth"], info["bmHeight"]),
                               bmp.GetBitmapBits(True), "raw", "BGRX", 0, 1)
    finally:
        # sempre devolve os recursos do Windows, senão eles acabam com o tempo
        if bmp:
            win32gui.DeleteObject(bmp.GetHandle())
        if dc_mem:
            dc_mem.DeleteDC()
        if dc:
            dc.DeleteDC()
        win32gui.ReleaseDC(hwnd, dc_janela)
    if not ok or img.getbbox() is None:      # algumas janelas não aceitam: pega da tela
        if so_janela:
            return None
        img = ImageGrab.grab(bbox=(x0, y0, x1, y1), all_screens=True)
    return img


def recortar(img, zoom, cx, cy):
    """Zoom do app novo: zoom 100 = janela inteira (até 800 = 8x); cx/cy = centro
    de 0 a 10000. Volta (recorte, cx efetivo, cy efetivo).

    O recorte tem o formato da tela do 3DS (5:3), então com zoom a imagem enche
    a tela, sem faixas pretas. O centro é ajustado para o recorte não sair da
    janela, e esse centro efetivo volta para o 3DS ficar em sincronia (senão,
    empurrando além da borda, o 3DS "andava" sozinho e a imagem travava na volta).
    """
    z = max(1.0, min(8.0, zoom / 100))
    W, H = img.size
    # "quadro" = a janela inteira encaixada no formato da tela do 3DS
    quadro_w = max(W, H * LARGURA / ALTURA)
    quadro_h = quadro_w * ALTURA / LARGURA
    rw, rh = quadro_w / z, quadro_h / z

    def ajustar(c, r, total):
        if r >= total:                     # o recorte é maior que a janela: centraliza
            return total / 2
        return min(max(c / 10000 * total, r / 2), total - r / 2)

    px = ajustar(max(0, min(10000, cx)), rw, W)
    py = ajustar(max(0, min(10000, cy)), rh, H)
    caixa = (round(px - rw / 2), round(py - rh / 2), round(px + rw / 2), round(py + rh / 2))
    # fora da janela o crop completa com preto (só acontece em zoom baixo)
    return img.crop(caixa), round(px / W * 10000), round(py / H * 10000), caixa


def regiao_inteira(W, H):
    """Sem zoom: a janela inteira cabe na tela do 3DS com faixas pretas. Volta o
    retângulo da janela (x, y, w, h) que a tela do 3DS mostra, faixas incluídas."""
    escala = min(LARGURA / W, ALTURA / H)
    rw, rh = LARGURA / escala, ALTURA / escala
    return (W - rw) / 2, (H - rh) / 2, rw, rh


def ponto_na_janela(regiao, tamanho, u, v):
    """(u, v) de 0 a 10000 na tela do 3DS -> (x, y) na janela; None se cair na faixa preta."""
    rx, ry, rw, rh = regiao
    x, y = rx + u / 10000 * rw, ry + v / 10000 * rh
    if 0 <= x < tamanho[0] and 0 <= y < tamanho[1]:
        return x, y
    return None


def cursor_na_janela(alvo):
    """Onde está o cursor do mouse, em pixels da janela; None se está fora dela ou
    escondido (jogos costumam esconder)."""
    try:
        flags, _, (x, y) = win32gui.GetCursorInfo()
    except Exception:
        return None
    if not flags & 1:                         # CURSOR_SHOWING
        return None
    x0, y0, x1, y1 = retangulo(alvo)
    if x0 <= x < x1 and y0 <= y < y1:
        return x - x0, y - y0
    return None


# seta do mouse (pontos em pixels da tela do 3DS)
SETA = [(0, 0), (0, 15), (4, 11), (7, 17), (10, 16), (7, 10), (12, 10)]


def desenhar_cursor(tela, x, y):
    pontos = [(x + px, y + py) for px, py in SETA]
    ImageDraw.Draw(tela).polygon(pontos, fill=(255, 255, 255), outline=(0, 0, 0))


def para_3ds(img, qualidade, cursor=None):
    """cursor: (x, y) em pixels de img, para desenhar a seta do mouse."""
    # aumenta ou diminui para caber em 400x240 (com zoom o recorte pode ser menor que a tela);
    # BILINEAR é bem mais rápido que LANCZOS e, em 400x240, fica igual
    escala = min(LARGURA / img.width, ALTURA / img.height)
    img = img.resize((max(1, round(img.width * escala)), max(1, round(img.height * escala))),
                     Image.BILINEAR)
    tela = Image.new("RGB", (LARGURA, ALTURA))
    dx, dy = (LARGURA - img.width) // 2, (ALTURA - img.height) // 2
    tela.paste(img, (dx, dy))
    # a seta tem sempre o mesmo tamanho na tela do 3DS (encolhida junto com a imagem sumiria)
    if cursor and 0 <= cursor[0] * escala < img.width and 0 <= cursor[1] * escala < img.height:
        desenhar_cursor(tela, round(dx + cursor[0] * escala), round(dy + cursor[1] * escala))
    saida = io.BytesIO()
    tela.save(saida, "JPEG", quality=qualidade)
    return saida.getvalue()


TECLAS = {"enter": Key.enter, "backspace": Key.backspace, "tab": Key.tab, "esc": Key.esc}


def na_frente(hwnd):
    """Traz a janela para a frente só se ela não estiver (e espera ela aparecer).
    Na tela inteira não traz nada: o clique vai para o que estiver no ponto."""
    if isinstance(hwnd, Tela):
        return
    if user32.GetForegroundWindow() != hwnd:
        trazer_para_frente(hwnd)
        time.sleep(0.05)


class Espelho:
    def __init__(self, config):
        self.config = config
        self.status = {"conectado": False, "janela": ""}

    def iniciar(self):
        self.servidor = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.servidor.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        self.servidor.bind(("0.0.0.0", PORTA))
        self.servidor.listen(1)
        threading.Thread(target=self._aceitar, daemon=True, name="espelho").start()

    def _aceitar(self):
        while True:
            try:
                conn, origem = self.servidor.accept()
            except OSError:
                time.sleep(1)
                continue
            print(f"Espelho: 3DS conectado ({origem[0]})")
            self.status["conectado"] = True
            try:
                self._atender(conn)
            except Exception as e:     # inclui erros do Windows (pywintypes.error)
                print("Espelho: desconectou", repr(e))
            finally:
                conn.close()
                self.status.update(conectado=False, janela="")

    def _atender(self, conn):
        conn.settimeout(300)          # digitando no teclado do 3DS o pedido demora
        arquivo = conn.makefile("rb")
        lista, escolhida = [], None
        regiao, tamanho = None, None     # o que o último quadro mostrou (para o toque)
        while True:
            linha = arquivo.readline()
            if not linha:
                return
            cmd = linha.decode("ascii", "ignore").strip()

            if cmd == "L":
                lista = listar_janelas()
                titulos = "\n".join(texto(t) for _, t in lista).encode("utf-8")
                conn.sendall(b"3DSL" + struct.pack("<I", len(titulos)) + titulos)

            elif cmd.startswith("S "):
                regiao = None                # o toque espera o primeiro quadro da janela nova
                try:
                    escolhida = lista[int(cmd[2:])][0]
                    self.status["janela"] = titulo_de(escolhida)
                except Exception:
                    escolhida = None

            elif cmd == "F" or cmd.startswith("F "):
                # "F" (app antigo) ou "F <zoom> <centro x> <centro y>" (app com zoom)
                com_zoom = len(cmd.split()) == 4
                try:
                    zoom, cx, cy = (int(n) for n in cmd.split()[1:4])
                except ValueError:
                    zoom, cx, cy = 100, 5000, 5000
                jpeg = b""
                if existe(escolhida):
                    try:
                        img = capturar_alvo(escolhida)
                        if img:
                            tamanho = img.size
                            cursor = cursor_na_janela(escolhida)
                            if zoom > 100:
                                img, cx, cy, (x0, y0, x1, y1) = recortar(img, zoom, cx, cy)
                                regiao = (x0, y0, x1 - x0, y1 - y0)
                                if cursor:
                                    cursor = (cursor[0] - x0, cursor[1] - y0)
                            else:
                                cx, cy = 5000, 5000    # janela inteira: centro no meio
                                regiao = regiao_inteira(*tamanho)
                            jpeg = para_3ds(img, self.config["espelho"]["qualidade_jpeg"], cursor)
                    except Exception as e:
                        print("Espelho: erro ao capturar", e)
                if not jpeg:
                    regiao = None
                if com_zoom:   # o app com zoom recebe também o centro efetivo
                    conn.sendall(b"3DSZ" + struct.pack("<IHH", len(jpeg), max(0, min(10000, cx)),
                                                       max(0, min(10000, cy))) + jpeg)
                else:
                    conn.sendall(b"3DSF" + struct.pack("<I", len(jpeg)) + jpeg)

            elif cmd.startswith(("T ", "R ", "M ")):
                # toque no 3DS: "T u v botao" = clique, "R u v passos" = rodinha,
                # "M u v" = só mover. u, v de 0 a 10000 na imagem que o 3DS está mostrando.
                partes = cmd.split()
                try:
                    u, v = int(partes[1]), int(partes[2])
                    n = int(partes[3]) if len(partes) > 3 else 0
                except (ValueError, IndexError):
                    continue
                if not (regiao and existe(escolhida)):
                    continue
                ponto = ponto_na_janela(regiao, tamanho, u, v)
                if not ponto:
                    continue                       # tocou na faixa preta
                esquerda, topo, _, _ = retangulo(escolhida)
                x, y = int(esquerda + ponto[0]), int(topo + ponto[1])
                try:
                    if cmd[0] == "M":
                        user32.SetCursorPos(x, y)
                        continue
                    na_frente(escolhida)
                    user32.SetCursorPos(x, y)
                    time.sleep(0.01)
                    if cmd[0] == "T":
                        mouse.click(Button.right if n == 2 else Button.left)
                    elif n:
                        mouse.scroll(0, max(-10, min(10, n)))
                except Exception as e:
                    print("Espelho: erro no toque", e)

            elif cmd.startswith(("K ", "E ")):
                # teclado do 3DS: "K <texto em hex>" digita, "E <tecla>" aperta enter etc.
                if not existe(escolhida):
                    continue
                try:
                    na_frente(escolhida)
                    if cmd[0] == "K":
                        teclado.type(bytes.fromhex(cmd[2:].strip()).decode("utf-8", "ignore"))
                    elif cmd[2:].strip() in TECLAS:
                        teclado.tap(TECLAS[cmd[2:].strip()])
                except Exception as e:
                    print("Espelho: erro no teclado", e)
