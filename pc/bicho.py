"""Bichinho: conta ao bichinho do 3DS o que o PC está fazendo.

O 3DS manda "3DSB" para a porta UDP 5014 a cada segundo e o PC responde
"3DSB" + linhas "chave=valor":
    cpu=34          uso da CPU (%)
    ram=61          memória usada (%)
    musica=1        tem música tocando (Spotify, YouTube...)
    titulo=...      nome da música
    ocioso=12       segundos sem mexer no mouse/teclado
    janelas=9       janelas abertas
    hora=14         hora do PC (o bichinho dorme de madrugada)
    skin=123456     número (crc) da skin, 0 = sem skin

Skin (opcional): imagens em pc/bicho/ trocam o desenho do bichinho. Os nomes dizem o
estado: normal_*, danca_*, dorme_*, bravo_*, calor_*, feliz_* (PNG com transparência ou
GIF animado; vários arquivos = quadros da animação, na ordem do nome). Sem fundo
transparente, a cor do canto de cima-esquerdo vira transparente.
O 3DS baixa a skin em pedaços e guarda no SD:
    "3DSb" + u16 parte  ->  "3DSb" + u32 crc + u16 parte + u16 total + pedaço
Pacote da skin: "SKN1" + u8 estados (6) + para cada estado: u8 quadros + para cada
quadro: u8 largura + u8 altura + pixels RGBA4444 (u16, R nos 4 bits de cima).
"""

import ctypes
import glob
import os
import socket
import struct
import zlib
import threading
import time

import psutil
import win32con
import win32gui
from PIL import Image, ImageSequence

PORTA = 5014
PASTA_SKIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bicho")
ESTADOS = ["normal", "danca", "dorme", "bravo", "calor", "feliz"]
MAX_QUADROS, MAX_LADO, PEDACO = 8, 64, 1000


def _quadros(caminho):
    img = Image.open(caminho)
    for q in ImageSequence.Iterator(img):
        q = q.convert("RGBA")
        if q.getextrema()[3][0] == 255:                  # sem transparência: o canto vira fundo
            fundo = q.getpixel((0, 0))
            q.putdata([(r, g, b, 0) if (r, g, b, a) == fundo else (r, g, b, a) for r, g, b, a in q.getdata()])
        caixa = q.getbbox()
        if caixa:
            q = q.crop(caixa)
            if max(q.size) > MAX_LADO:                    # grande demais: reduz (pixel art: sem borrar)
                f = MAX_LADO / max(q.size)
                q = q.resize((max(1, int(q.width * f)), max(1, int(q.height * f))), Image.NEAREST)
            yield q


def montar_skin():
    """O pacote da skin (bytes), ou b"" se a pasta não tem imagens."""
    if not os.path.isdir(PASTA_SKIN):
        return b""
    corpo, algum = b"SKN1" + bytes([len(ESTADOS)]), False
    for estado in ESTADOS:
        arquivos = sorted(glob.glob(os.path.join(PASTA_SKIN, estado + "*.png")) +
                          glob.glob(os.path.join(PASTA_SKIN, estado + "*.gif")))
        quadros = [q for a in arquivos for q in _quadros(a)][:MAX_QUADROS]
        algum |= bool(quadros)
        corpo += bytes([len(quadros)])
        for q in quadros:
            px = bytearray()
            for r, g, b, a in q.getdata():
                px += struct.pack("<H", (r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | (a >> 4))
            corpo += bytes([q.width, q.height]) + px
    return corpo if algum else b""


class _UltimaEntrada(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_uint), ("dwTime", ctypes.c_uint)]


def segundos_ocioso():
    info = _UltimaEntrada(ctypes.sizeof(_UltimaEntrada), 0)
    if not ctypes.windll.user32.GetLastInputInfo(ctypes.byref(info)):
        return 0
    return max(0, (ctypes.windll.kernel32.GetTickCount() - info.dwTime) // 1000)


def contar_janelas():
    n = 0

    def cb(hwnd, _):
        nonlocal n
        if (win32gui.IsWindowVisible(hwnd) and not win32gui.IsIconic(hwnd) and win32gui.GetWindowText(hwnd)
                and not win32gui.GetWindowLong(hwnd, win32con.GWL_EXSTYLE) & win32con.WS_EX_TOOLWINDOW):
            x0, y0, x1, y1 = win32gui.GetWindowRect(hwnd)
            if x1 - x0 >= 150 and y1 - y0 >= 100:
                n += 1
        return True

    win32gui.EnumWindows(cb, None)
    return n


class Bicho:
    def __init__(self, config):
        self.config = config
        self.status = {"conectado": False}
        self.resposta = b"3DSB"
        self.ultimo_pedido = 0.0
        self.skin, self.skin_crc, self.skin_mtime = b"", 0, None

    def iniciar(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", PORTA))
        self.sock.settimeout(1)
        threading.Thread(target=self._medir, daemon=True, name="bicho-medir").start()
        threading.Thread(target=self._loop, daemon=True, name="bicho").start()

    def _medir(self):
        """Mede uma vez por segundo, só enquanto o bichinho está aberto no 3DS."""
        from tela import Midia, texto          # a mesma leitura de música da Segunda Tela
        midia = Midia()
        psutil.cpu_percent()
        while True:
            time.sleep(1)
            if time.monotonic() - self.ultimo_pedido > 5:
                continue
            self._conferir_skin()
            try:
                titulo, _artista, tocando = midia.ler()
                linhas = {
                    "cpu": round(psutil.cpu_percent()),
                    "ram": round(psutil.virtual_memory().percent),
                    "musica": tocando,
                    "titulo": texto(titulo, 40),
                    "ocioso": segundos_ocioso(),
                    "janelas": contar_janelas(),
                    "hora": time.localtime().tm_hour,
                    "skin": self.skin_crc,
                }
                self.resposta = b"3DSB" + "".join(f"{k}={v}\n" for k, v in linhas.items()).encode("utf-8")
            except Exception as e:
                print("Bichinho: erro ao medir o PC", repr(e))

    def _conferir_skin(self):
        """Remonta a skin quando algo muda na pasta pc/bicho."""
        try:
            arquivos = sorted(glob.glob(os.path.join(PASTA_SKIN, "*")))
            marca = tuple((a, os.path.getmtime(a)) for a in arquivos)
        except OSError:
            marca = ()
        if marca == self.skin_mtime:
            return
        self.skin_mtime = marca
        try:
            self.skin = montar_skin()
        except Exception as e:
            print("Bichinho: erro na skin", repr(e))
            self.skin = b""
        self.skin_crc = zlib.crc32(self.skin) & 0xFFFFFFFF if self.skin else 0
        if self.skin:
            print(f"Bichinho: skin com {len(self.skin)} bytes (crc {self.skin_crc})")

    def _loop(self):
        while True:
            try:
                dados, origem = self.sock.recvfrom(64)
            except (socket.timeout, OSError):
                self.status["conectado"] = time.monotonic() - self.ultimo_pedido < 5
                continue
            if dados[:4] == b"3DSb" and len(dados) >= 6:    # um pedaço da skin
                parte = struct.unpack("<H", dados[4:6])[0]
                total = max(1, (len(self.skin) + PEDACO - 1) // PEDACO)
                pedaco = self.skin[parte * PEDACO:(parte + 1) * PEDACO]
                try:
                    self.sock.sendto(b"3DSb" + struct.pack("<IHH", self.skin_crc, parte, total) + pedaco, origem)
                except OSError:
                    pass
                continue
            if dados[:4] != b"3DSB":
                continue
            if not self.status["conectado"]:
                print(f"Bichinho: 3DS conectado ({origem[0]})")
            self.status["conectado"] = True
            self.ultimo_pedido = time.monotonic()
            try:
                self.sock.sendto(self.resposta, origem)
            except OSError:
                pass
