"""Coisas usadas por vários módulos do 3DS Hub."""

import ctypes
import socket
import time

from pynput.keyboard import Controller as Teclado, Key, KeyCode
from pynput.mouse import Button, Controller as Mouse

teclado = Teclado()
mouse = Mouse()

ESPECIAIS = {
    "space": Key.space, "espaco": Key.space,
    "shift": Key.shift, "ctrl": Key.ctrl, "alt": Key.alt, "win": Key.cmd,
    "tab": Key.tab, "esc": Key.esc, "enter": Key.enter,
    "backspace": Key.backspace, "delete": Key.delete,
    "up": Key.up, "down": Key.down, "left": Key.left, "right": Key.right,
    "home": Key.home, "end": Key.end, "pageup": Key.page_up, "pagedown": Key.page_down,
    **{f"f{i}": getattr(Key, f"f{i}") for i in range(1, 13)},
}
BOTOES_MOUSE = {
    "mouse_esquerdo": Button.left,
    "mouse_direito": Button.right,
    "mouse_meio": Button.middle,
}


def tecla(nome):
    """'space' -> Key.space, 'e' -> KeyCode('e'), 'mouse_esquerdo' -> Button.left."""
    nome = (nome or "").strip().lower()
    if nome in BOTOES_MOUSE:
        return BOTOES_MOUSE[nome]
    if nome in ESPECIAIS:
        return ESPECIAIS[nome]
    if len(nome) == 1:
        return KeyCode.from_char(nome)
    return None


def apertar(t):
    (mouse.press if isinstance(t, Button) else teclado.press)(t)


def soltar(t):
    (mouse.release if isinstance(t, Button) else teclado.release)(t)


class _MouseInput(ctypes.Structure):
    _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long), ("mouseData", ctypes.c_ulong),
                ("dwFlags", ctypes.c_ulong), ("time", ctypes.c_ulong),
                ("dwExtraInfo", ctypes.c_size_t)]


class _Input(ctypes.Structure):
    class _U(ctypes.Union):
        # o union do Windows tem o tamanho do maior membro (o de teclado/hardware é menor)
        _fields_ = [("mi", _MouseInput), ("_pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", ctypes.c_ulong), ("u", _U)]


# Como mover o mouse, por perfil ("mouse" no perfil; o Controle troca a cada pacote):
#   False ("relativo", padrão): como um mouse de verdade (SendInput). Precisa nos jogos
#       que leem o mouse direto (Raw Input, ex. Minecraft), que ignoram o teleporte.
#   True ("cursor"): teleporta o cursor (SetCursorPos). Precisa no LoL, que ignora o
#       mouse simulado (testado: SendInput não mexe nada com o jogo na frente).
modo_cursor = False


def mover(dx, dy):
    """Move o mouse dx, dy pixels, do jeito escolhido em modo_cursor."""
    if not dx and not dy:
        return
    if modo_cursor:
        p = _Ponto()
        ctypes.windll.user32.GetCursorPos(ctypes.byref(p))
        ctypes.windll.user32.SetCursorPos(p.x + int(dx), p.y + int(dy))
        return
    entrada = _Input(type=0)                       # INPUT_MOUSE
    entrada.mi = _MouseInput(dx=int(dx), dy=int(dy), dwFlags=0x0001)   # MOUSEEVENTF_MOVE
    ctypes.windll.user32.SendInput(1, ctypes.byref(entrada), ctypes.sizeof(_Input))


def trazer_para_frente(hwnd):
    """Coloca a janela na frente. O Windows só deixa um programa em segundo plano
    fazer isso "emprestando" a vez de quem está na frente (AttachThreadInput)."""
    user32 = ctypes.windll.user32
    frente = user32.GetForegroundWindow()
    if frente == hwnd:
        return
    meu = ctypes.windll.kernel32.GetCurrentThreadId()
    dele = user32.GetWindowThreadProcessId(frente, None)
    user32.AttachThreadInput(meu, dele, True)
    user32.SetForegroundWindow(hwnd)
    user32.BringWindowToTop(hwnd)
    user32.AttachThreadInput(meu, dele, False)


class _Ponto(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def _no_ponto(x, y, hwnd, acao):
    """Traz a janela para a frente, leva o mouse até (x, y), faz a ação e
    devolve o mouse para onde estava."""
    user32 = ctypes.windll.user32
    trazer_para_frente(hwnd)
    time.sleep(0.05)                      # dá tempo da janela aparecer na frente
    antes = _Ponto()
    user32.GetCursorPos(ctypes.byref(antes))
    user32.SetCursorPos(int(x), int(y))
    time.sleep(0.01)
    acao()
    time.sleep(0.02)
    user32.SetCursorPos(antes.x, antes.y)


def clicar_em(x, y, botao, hwnd):
    """Clique de verdade em (x, y) da tela: botao 1 = esquerdo, 2 = direito."""
    _no_ponto(x, y, hwnd, lambda: mouse.click(Button.right if botao == 2 else Button.left))


def rolar_em(x, y, passos, hwnd):
    """Roda a rodinha do mouse em (x, y): positivo = sobe, negativo = desce."""
    _no_ponto(x, y, hwnd, lambda: mouse.scroll(0, passos))


def combinacao(texto):
    """Aperta e solta uma combinação como 'ctrl+shift+m'."""
    teclas = [tecla(p) for p in texto.split("+")]
    teclas = [t for t in teclas if t is not None]
    for t in teclas:
        apertar(t)
    for t in reversed(teclas):
        soltar(t)


HOTSPOT = "192.168.137."   # faixa que o Hotspot móvel do Windows usa


def ips_locais():
    """[(nome da rede, ip)] de todas as redes do PC, com o hotspot primeiro.
    O ip_local() só mostra a rede principal; com o 3DS no hotspot, o IP certo é outro."""
    import psutil
    ips = []
    for nome, enderecos in psutil.net_if_addrs().items():
        for e in enderecos:
            if e.family == socket.AF_INET and not e.address.startswith(("127.", "169.254.")):
                ips.append(("Hotspot" if e.address.startswith(HOTSPOT) else nome, e.address))
    ips.sort(key=lambda p: not p[1].startswith(HOTSPOT))
    return ips


def ip_para_o_3ds():
    """O IP que o 3DS deve usar: o do hotspot, se estiver ligado; senão o da rede principal."""
    for nome, ip in ips_locais():
        if ip.startswith(HOTSPOT):
            return ip
    return ip_local()


def ip_local():
    """Descobre o IP do PC na rede local (não envia nada)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "desconhecido"
    finally:
        s.close()
