"""Gera a arte do 3DS Hub: icon.png (48x48), banner.png (256x128) e banner.wav.

    py gerar_arte.py

O icon.png vai para a pasta 3ds (o Makefile usa no .3dsx) e os três vão para o CIA.
"""

import math
import os
import struct
import wave

from PIL import Image, ImageDraw, ImageFont

AQUI = os.path.dirname(os.path.abspath(__file__))
FUNDO = (21, 24, 35)
CARTAO = (32, 37, 58)
CORES = [(127, 119, 221), (29, 158, 117), (212, 83, 126), (216, 90, 48), (55, 138, 221)]


def fonte(tamanho, negrito=True):
    for nome in (("segoeuib.ttf" if negrito else "segoeui.ttf"), "arialbd.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(os.path.join(os.environ.get("WINDIR", "C:/Windows"), "Fonts", nome), tamanho)
        except OSError:
            pass
    return ImageFont.load_default()


def controle(d, cx, cy, s, cor, fundo):
    """O mesmo controle desenhado no app (grade de 24 unidades)."""
    u = s / 24
    d.rounded_rectangle((cx - 10 * u, cy - 6 * u, cx + 10 * u, cy + 5 * u), radius=5 * u, fill=cor)
    for gx in (-7, 7):
        d.ellipse((cx + (gx - 4) * u, cy - 0.5 * u, cx + (gx + 4) * u, cy + 7.5 * u), fill=cor)
    d.rectangle((cx - 9 * u, cy - 1.5 * u, cx - 4 * u, cy + 0.5 * u), fill=fundo)
    d.rectangle((cx - 7.5 * u, cy - 3 * u, cx - 5.5 * u, cy + 2 * u), fill=fundo)
    for bx, by in ((5.5, -2), (8, 0.3)):
        d.ellipse((cx + (bx - 1.3) * u, cy + (by - 1.3) * u, cx + (bx + 1.3) * u, cy + (by + 1.3) * u), fill=fundo)


def icone():
    # desenha em 4x e reduz: bordas suaves
    e = 4
    img = Image.new("RGBA", (48 * e, 48 * e), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, 48 * e - 1, 48 * e - 1), radius=10 * e, fill=FUNDO)
    controle(d, 24 * e, 21 * e, 34 * e, (244, 245, 250), FUNDO)
    for i, cor in enumerate(CORES):                    # uma bolinha por módulo
        x = (9 + i * 7.5) * e
        d.ellipse((x - 2.6 * e, 37 * e - 2.6 * e, x + 2.6 * e, 37 * e + 2.6 * e), fill=cor)
    img = img.resize((48, 48), Image.LANCZOS)
    img.save(os.path.join(AQUI, "icon.png"))
    img.save(os.path.join(AQUI, "..", "icon.png"))      # o Makefile usa este no .3dsx


def banner():
    e = 2
    img = Image.new("RGB", (256 * e, 128 * e), FUNDO)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((14 * e, 30 * e, 82 * e, 98 * e), radius=16 * e, fill=CORES[0])
    controle(d, 48 * e, 62 * e, 46 * e, (244, 245, 250), CORES[0])
    d.text((96 * e, 34 * e), "3DS Hub", font=fonte(30 * e), fill=(232, 234, 242))
    d.text((97 * e, 72 * e), "Controle, Segunda Tela,", font=fonte(11 * e, False), fill=(184, 189, 208))
    d.text((97 * e, 86 * e), "Sintetizador, Câmera e Espelho", font=fonte(11 * e, False), fill=(184, 189, 208))
    for i, cor in enumerate(CORES):
        d.rounded_rectangle(((97 + i * 30) * e, 106 * e, (121 + i * 30) * e, 111 * e), radius=2 * e, fill=cor)
    img.resize((256, 128), Image.LANCZOS).save(os.path.join(AQUI, "banner.png"))


def som():
    """Um 'plim' de duas notas (como o som de abrir um app no 3DS)."""
    taxa, dur = 22050, 1.1
    amostras = []
    for i in range(int(taxa * dur)):
        t = i / taxa
        v = 0.0
        for inicio, freq in ((0.0, 880.0), (0.12, 1318.5)):
            if t >= inicio:
                tt = t - inicio
                v += math.sin(2 * math.pi * freq * tt) * math.exp(-tt * 5) * 0.35
        amostras.append(int(max(-1, min(1, v)) * 32767))
    with wave.open(os.path.join(AQUI, "banner.wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(taxa)
        w.writeframes(struct.pack(f"<{len(amostras)}h", *amostras))


if __name__ == "__main__":
    icone()
    banner()
    som()
    print("arte gerada em", AQUI)
