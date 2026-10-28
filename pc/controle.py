"""Controle: o 3DS vira controle de Xbox 360 (ou teclado) + touchpad + mira por giroscópio."""

import socket
import struct
import threading
import time

import psutil
import vgamepad as vg
import win32gui
import win32process

from util import Button, apertar, mouse, mover, soltar, tecla

PORTA = 5005
TIMEOUT_S = 0.5

# pacote novo (3DS Hub) e o antigo (app PadRemoto), os dois funcionam
FORMATO = "<4sIIhhhhHHBBhhh"    # ... tocando, flags, giro guinada, giro inclinação, giro rolagem
FORMATO_ANTIGO = "<4sIIhhhhHHB"
TAMANHO = struct.calcsize(FORMATO)
TAMANHO_ANTIGO = struct.calcsize(FORMATO_ANTIGO)

FLAG_GIRO = 1 << 0
FLAG_TECLADO = 1 << 1

# bits do libctru
BITS = {
    "A": 1 << 0, "B": 1 << 1, "SELECT": 1 << 2, "START": 1 << 3,
    "DIREITA": 1 << 4, "ESQUERDA": 1 << 5, "CIMA": 1 << 6, "BAIXO": 1 << 7,
    "R": 1 << 8, "L": 1 << 9, "X": 1 << 10, "Y": 1 << 11,
    "ZL": 1 << 14, "ZR": 1 << 15,
}

XB = vg.XUSB_BUTTON
MAPA_XBOX = {
    "A": XB.XUSB_GAMEPAD_A, "B": XB.XUSB_GAMEPAD_B,
    "X": XB.XUSB_GAMEPAD_X, "Y": XB.XUSB_GAMEPAD_Y,
    "START": XB.XUSB_GAMEPAD_START, "SELECT": XB.XUSB_GAMEPAD_BACK,
    "CIMA": XB.XUSB_GAMEPAD_DPAD_UP, "BAIXO": XB.XUSB_GAMEPAD_DPAD_DOWN,
    "ESQUERDA": XB.XUSB_GAMEPAD_DPAD_LEFT, "DIREITA": XB.XUSB_GAMEPAD_DPAD_RIGHT,
}

CIRCLE_MAX = 150
CSTICK_MAX = 140
ZONA_MORTA = 15
LIMIAR_DIRECIONAL = 60   # circle pad -> WASD/setas a partir desse valor


def eixo(valor, maximo):
    if abs(valor) < ZONA_MORTA:
        return 0
    valor = max(-maximo, min(maximo, valor))
    return int(valor / maximo * 32767)


# ---------------------------------------------------------------- perfis
class DetectorPerfil:
    """Vê qual programa está em primeiro plano e escolhe o perfil."""

    def __init__(self, config):
        self.config = config
        self.processo = ""
        self._ultimo = 0.0

    def perfil(self):
        agora = time.monotonic()
        if agora - self._ultimo > 0.5:
            self._ultimo = agora
            try:
                _, pid = win32process.GetWindowThreadProcessId(win32gui.GetForegroundWindow())
                self.processo = psutil.Process(pid).name().lower()
            except Exception:
                self.processo = ""
        perfis = self.config["perfis"] or [{"nome": "Padrao", "modo": "controle"}]
        for p in perfis[1:]:
            if self.processo in [str(n).lower() for n in p.get("processos", [])]:
                return p
        return perfis[0]


# ---------------------------------------------------------------- touchpad
TAP_MAX_S = 0.20
SEGURAR_S = 0.60
MOV_MAX_TAP = 6
JANELA_DUPLO_S = 0.25
FAIXA_ROLAGEM_X = 280
SENS_ROLAGEM = 0.12


class Touchpad:
    """toque = clique, 2 toques ou segurar = clique direito, toque+segurar = arrastar,
    faixa da direita = rolagem."""

    def __init__(self, config):
        self.config = config
        self.tocando = False
        self.ultimo_tap = 0.0
        self.clique_pendente = False
        self._zerar()

    def _zerar(self):
        self.x = self.y = 0
        self.inicio = 0.0
        self.mov_total = 0
        self.modo = "mouse"
        self.clicou_direito = False
        self.arrastando = False
        self.segundo_toque = False
        self.acc_x = self.acc_y = self.acc_rolagem = 0.0

    def atualizar(self, tocando, x, y):
        agora = time.monotonic()
        sens = self.config["controle"]["sensibilidade_toque"]

        if (self.clique_pendente and not tocando
                and agora - self.ultimo_tap > JANELA_DUPLO_S):
            mouse.click(Button.left)
            self.clique_pendente = False

        if tocando and not self.tocando:
            self._zerar()
            self.x, self.y, self.inicio = x, y, agora
            self.modo = "rolagem" if x >= FAIXA_ROLAGEM_X else "mouse"
            if self.clique_pendente and agora - self.ultimo_tap > JANELA_DUPLO_S:
                mouse.click(Button.left)
                self.clique_pendente = False
            if self.modo == "mouse" and self.clique_pendente:
                self.segundo_toque = True
                self.clique_pendente = False

        elif tocando:
            dx, dy = x - self.x, y - self.y
            self.x, self.y = x, y
            self.mov_total += abs(dx) + abs(dy)

            if self.segundo_toque and (self.mov_total >= MOV_MAX_TAP
                                       or agora - self.inicio > TAP_MAX_S):
                self.segundo_toque = False
                self.arrastando = True
                mouse.press(Button.left)

            if self.modo == "mouse":
                self.acc_x += dx * sens
                self.acc_y += dy * sens
                mx, my = int(self.acc_x), int(self.acc_y)
                if mx or my:
                    mover(mx, my)
                    self.acc_x -= mx
                    self.acc_y -= my
                if (not self.clicou_direito and not self.arrastando
                        and not self.segundo_toque and self.mov_total < MOV_MAX_TAP
                        and agora - self.inicio > SEGURAR_S):
                    mouse.click(Button.right)
                    self.clicou_direito = True
            else:
                self.acc_rolagem += -dy * SENS_ROLAGEM
                passos = int(self.acc_rolagem)
                if passos:
                    mouse.scroll(0, passos)
                    self.acc_rolagem -= passos

        elif self.tocando:
            duracao = agora - self.inicio
            if self.arrastando:
                mouse.release(Button.left)
            elif self.segundo_toque:
                mouse.click(Button.right)
            elif (self.modo == "mouse" and not self.clicou_direito
                  and duracao < TAP_MAX_S and self.mov_total < MOV_MAX_TAP):
                self.clique_pendente = True
                self.ultimo_tap = agora
            self._zerar()

        self.tocando = tocando

    def soltar_tudo(self):
        if self.arrastando:
            mouse.release(Button.left)
        self.tocando = False
        self.clique_pendente = False
        self._zerar()


# ---------------------------------------------------------------- modo teclado
class Teclas:
    """Mantém apertadas exatamente as teclas pedidas, soltando as outras."""

    def __init__(self):
        self.apertadas = set()

    def aplicar(self, desejadas):
        for t in self.apertadas - desejadas:
            soltar(t)
        for t in desejadas - self.apertadas:
            apertar(t)
        self.apertadas = desejadas

    def soltar_tudo(self):
        self.aplicar(set())


DIRECOES = {
    "wasd": ("w", "s", "a", "d"),
    "setas": ("up", "down", "left", "right"),
}


def teclas_desejadas(keys, cx, cy, cfg_teclado):
    desejadas = set()
    for nome, valor in cfg_teclado.get("teclas", {}).items():
        bit = BITS.get(str(nome).upper())
        t = tecla(str(valor))
        if bit and t is not None and keys & bit:
            desejadas.add(t)
    direcoes = DIRECOES.get(cfg_teclado.get("analogico"))
    if direcoes:
        cima, baixo, esq, dir_ = (tecla(n) for n in direcoes)
        if cy > LIMIAR_DIRECIONAL: desejadas.add(cima)
        if cy < -LIMIAR_DIRECIONAL: desejadas.add(baixo)
        if cx < -LIMIAR_DIRECIONAL: desejadas.add(esq)
        if cx > LIMIAR_DIRECIONAL: desejadas.add(dir_)
    return desejadas


# ---------------------------------------------------------------- legenda
# O que cada botão faz no perfil ativo, para a tela de cima do 3DS mostrar.
# O perfil pode ter "legenda": {"A": "Pular", ...} com descrições próprias;
# sem isso, mostra o nome da tecla (modo teclado) ou do botão do Xbox.

NOMES_TECLAS = {
    "space": "Espaço", "espaco": "Espaço", "shift": "Shift", "ctrl": "Ctrl", "alt": "Alt",
    "win": "Windows", "tab": "Tab", "esc": "Esc", "enter": "Enter",
    "backspace": "Backspace", "delete": "Delete", "home": "Home", "end": "End",
    "pageup": "Page Up", "pagedown": "Page Down",
    "up": "Seta cima", "down": "Seta baixo", "left": "Seta esquerda", "right": "Seta direita",
    "mouse_esquerdo": "Clique esquerdo", "mouse_direito": "Clique direito", "mouse_meio": "Clique do meio",
}
NOMES_ANALOGICO = {"wasd": "WASD", "setas": "Setas", "mouse": "Mouse", "nada": "-"}
BOTOES_LEGENDA = ("L", "R", "ZL", "ZR", "A", "B", "X", "Y", "CIMA", "BAIXO", "ESQUERDA", "DIREITA",
                  "START", "SELECT", "CIRCLE", "CSTICK")


def nome_tecla(valor):
    v = str(valor or "").strip().lower()
    if not v:
        return "-"
    return NOMES_TECLAS.get(v, v.upper() if len(v) <= 3 else v.capitalize())


def legenda(perfil, teclado, cliques, config):
    """Texto 'BOTAO=descrição' por linha, para o 3DS."""
    textos = {}
    if teclado:
        cfg_teclado = perfil if perfil.get("modo") == "teclado" else config["teclado_padrao"]
        teclas = {str(k).upper(): v for k, v in cfg_teclado.get("teclas", {}).items()}
        for nome in BOTOES_LEGENDA[:14]:
            textos[nome] = nome_tecla(teclas[nome]) if nome in teclas else "-"
        textos["CIRCLE"] = NOMES_ANALOGICO.get(cfg_teclado.get("analogico"), "-")
        textos["CSTICK"] = "Mouse"
        extras = cfg_teclado.get("legenda", {})
    else:
        gatilhos = perfil.get("l_r_como_gatilhos", False)
        textos.update({
            "A": "A", "B": "B", "X": "X", "Y": "Y", "START": "Start", "SELECT": "Back",
            "CIMA": "Direcional", "BAIXO": "Direcional", "ESQUERDA": "Direcional", "DIREITA": "Direcional",
            "L": "LT" if gatilhos else "LB", "R": "RT" if gatilhos else "RB", "ZL": "LT", "ZR": "RT",
            "CIRCLE": "Analógico esquerdo", "CSTICK": "Analógico direito",
        })
        if cliques:              # giroscópio no mouse: L/R viram os cliques
            textos["L"], textos["R"] = "Clique esquerdo", "Clique direito"
        extras = perfil.get("legenda", {})
    for k, v in extras.items():  # descrições próprias do perfil
        if str(k).upper() in textos:
            textos[str(k).upper()] = str(v)
    return "".join(f"{k}={str(textos[k])[:24]}\n" for k in BOTOES_LEGENDA)


# ---------------------------------------------------------------- servidor
class Controle:
    def __init__(self, config):
        self.config = config
        self.status = {"conectado": False, "perfil": "", "ip": ""}
        self.pad = vg.VX360Gamepad()   # existe desde o início, antes de qualquer jogo abrir
        self.touch = Touchpad(config)
        self.teclas = Teclas()
        self.cliques = Teclas()        # L/R como cliques do mouse (giroscópio no modo controle)
        self.detector = DetectorPerfil(config)
        self.acc_giro_x = self.acc_giro_y = 0.0
        self.acc_cam_x = self.acc_cam_y = 0.0
        self.legenda_texto, self.legenda_tempo = "", 0.0   # última legenda mandada ao 3DS

    def iniciar(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", PORTA))
        self.sock.settimeout(TIMEOUT_S)
        threading.Thread(target=self._loop, daemon=True, name="controle").start()

    def soltar_tudo(self):
        self.pad.reset()
        self.pad.update()
        self.touch.soltar_tudo()
        self.teclas.soltar_tudo()
        self.cliques.soltar_tudo()

    def _gamepad(self, perfil, keys, cx, cy, sx, sy, giro_x, giro_y, cliques=False):
        if cliques:                     # L e R estão sendo os cliques do mouse
            keys &= ~(BITS["L"] | BITS["R"])
        gatilhos = perfil.get("l_r_como_gatilhos", False)
        for nome, botao in MAPA_XBOX.items():
            (self.pad.press_button if keys & BITS[nome] else self.pad.release_button)(button=botao)
        for nome, botao in (("L", XB.XUSB_GAMEPAD_LEFT_SHOULDER), ("R", XB.XUSB_GAMEPAD_RIGHT_SHOULDER)):
            (self.pad.press_button if keys & BITS[nome] and not gatilhos else self.pad.release_button)(button=botao)
        lt = keys & BITS["ZL"] or (gatilhos and keys & BITS["L"])
        rt = keys & BITS["ZR"] or (gatilhos and keys & BITS["R"])
        self.pad.left_trigger(value=255 if lt else 0)
        self.pad.right_trigger(value=255 if rt else 0)
        self.pad.left_joystick(x_value=eixo(cx, CIRCLE_MAX), y_value=eixo(cy, CIRCLE_MAX))
        rx = max(-32768, min(32767, eixo(sx, CSTICK_MAX) + giro_x))
        ry = max(-32768, min(32767, eixo(sy, CSTICK_MAX) + giro_y))
        self.pad.right_joystick(x_value=rx, y_value=ry)
        self.pad.update()

    def _giro(self, perfil, guinada, inclinacao, dt):
        """Converte o giroscópio (graus/s) em movimento de mouse ou analógico."""
        cfg = self.config["controle"]
        h = -guinada / 10.0      # girar para a esquerda = mira para a esquerda
        v = -inclinacao / 10.0
        if cfg.get("giro_inverter_horizontal"): h = -h
        if cfg.get("giro_inverter_vertical"): v = -v
        # zona morta: tremida da mão e o desvio que sobra da calibração não mexem a mira;
        # acima dela o movimento começa do zero, sem pulo
        zm = cfg.get("giro_zona_morta", 3)
        h = 0.0 if abs(h) < zm else h - zm * (1 if h > 0 else -1)
        v = 0.0 if abs(v) < zm else v - zm * (1 if v > 0 else -1)

        if perfil.get("giroscopio", "mouse") == "analogico_direito":
            maximo = cfg.get("giro_analogico_graus", 120)
            return (int(max(-1, min(1, h / maximo)) * 32767),
                    int(max(-1, min(1, -v / maximo)) * 32767))

        ppg = cfg.get("giro_pixels_por_grau", 20)
        self.acc_giro_x += h * dt * ppg
        self.acc_giro_y += v * dt * ppg
        mx, my = int(self.acc_giro_x), int(self.acc_giro_y)
        if mx or my:
            mover(mx, my)
            self.acc_giro_x -= mx
            self.acc_giro_y -= my
        return 0, 0

    def _loop(self):
        ultimo_seq = None
        ultimo_ack = 0.0
        modo_antes = None

        while True:
            try:
                dados, origem = self.sock.recvfrom(64)
            except socket.timeout:
                if self.status["conectado"]:
                    print("Controle: sem sinal do 3DS, soltando tudo.")
                    self.soltar_tudo()
                    self.status["conectado"] = False
                    ultimo_seq = None
                continue
            except OSError:
                continue

            if len(dados) == TAMANHO and dados[:4] == b"3DS2":
                # O app do 3DS manda os eixos com nomes trocados: o campo "inclinação"
                # (eixo Y do giroscópio) é o giro de lado, tipo volante, e o "rolagem"
                # (eixo X) é a inclinação para frente/trás. Aqui a mira usa o eixo certo.
                (_, seq, keys, cx, cy, sx, sy, tx, ty, tocando,
                 flags, guinada, _volante, inclinacao) = struct.unpack(FORMATO, dados)
            elif len(dados) == TAMANHO_ANTIGO and dados[:4] == b"3DSR":
                _, seq, keys, cx, cy, sx, sy, tx, ty, tocando = struct.unpack(FORMATO_ANTIGO, dados)
                flags = guinada = inclinacao = 0
            else:
                continue

            if ultimo_seq is not None and seq <= ultimo_seq and ultimo_seq - seq < 1000:
                continue
            ultimo_seq = seq

            if not self.status["conectado"]:
                print(f"Controle: 3DS conectado ({origem[0]})")
                self.status.update(conectado=True, ip=origem[0])

            try:
                modo_antes, ultimo_ack = self._processar(
                    origem, keys, cx, cy, sx, sy, tx, ty, tocando, flags,
                    guinada, inclinacao, modo_antes, ultimo_ack)
            except Exception as e:
                # um config.json estranho não pode derrubar o controle inteiro
                print("Controle: erro ao processar pacote:", repr(e))
                time.sleep(0.5)

    def _processar(self, origem, keys, cx, cy, sx, sy, tx, ty, tocando, flags,
                   guinada, inclinacao, modo_antes, ultimo_ack):
        agora = time.monotonic()
        # O 3DS manda um pacote por quadro (60/s). Usar 1/60 fixo evita que
        # pacotes que chegam em rajada pelo Wi-Fi distorçam o giroscópio.
        dt = 1 / 60

        perfil = self.detector.perfil()
        teclado = perfil.get("modo") == "teclado" or flags & FLAG_TECLADO
        modo = "teclado" if teclado else "controle"
        if modo != modo_antes:          # trocou de modo: nada fica preso
            self.soltar_tudo()
            modo_antes = modo
        self.status["perfil"] = f"{perfil.get('nome', '?')} ({modo})"

        cfg = self.config["controle"]
        giro_ligado = bool(flags & FLAG_GIRO)
        giro_x = giro_y = 0
        if giro_ligado and tocando and cfg.get("giro_pausar_no_toque", True):
            # dedo na tela = "levantar o mouse": o giroscópio pausa e dá pra
            # reposicionar o 3DS sem a câmera voltar junto
            self.acc_giro_x = self.acc_giro_y = 0.0
        elif giro_ligado:
            giro_x, giro_y = self._giro(perfil, guinada, inclinacao, dt)

        # no modo controle, com o giroscópio mexendo o mouse, L e R viram os cliques
        cliques = (not teclado and giro_ligado and perfil.get("giroscopio", "mouse") == "mouse"
                   and cfg.get("giro_l_r_como_cliques", True))
        self.cliques.aplicar({b for nome, b in (("L", Button.left), ("R", Button.right))
                              if cliques and keys & BITS[nome]})

        if teclado:
            cfg_teclado = perfil if perfil.get("modo") == "teclado" else self.config["teclado_padrao"]
            self.teclas.aplicar(teclas_desejadas(keys, cx, cy, cfg_teclado))
            if cfg_teclado.get("analogico") == "mouse":
                mover(int(eixo(cx, CIRCLE_MAX) / 32767 * 12),
                      int(-eixo(cy, CIRCLE_MAX) / 32767 * 12))
            # analógico direito (C-stick ou tela de toque no modo câmera) gira a câmera
            vel = self.config["controle"].get("camera_pixels_por_quadro", 15)
            self.acc_cam_x += eixo(sx, CSTICK_MAX) / 32767 * vel
            self.acc_cam_y -= eixo(sy, CSTICK_MAX) / 32767 * vel
            mx, my = int(self.acc_cam_x), int(self.acc_cam_y)
            if mx or my:
                mover(mx, my)
                self.acc_cam_x -= mx
                self.acc_cam_y -= my
        else:
            self._gamepad(perfil, keys, cx, cy, sx, sy, giro_x, giro_y, cliques)

        self.touch.atualizar(bool(tocando), tx, ty)

        if agora - ultimo_ack > 0.2:
            resposta = f"{str(perfil.get('nome', '?'))[:20]}\n{modo}".encode("ascii", "ignore")
            self.sock.sendto(b"3DSA" + resposta, origem)

        # legenda dos botões (tela de cima do 3DS): quando muda e a cada segundo
        texto = legenda(perfil, teclado, cliques, self.config)
        if texto != self.legenda_texto or agora - self.legenda_tempo > 1.0:
            self.sock.sendto(b"3DSM" + texto.encode("utf-8"), origem)
            self.legenda_texto, self.legenda_tempo = texto, agora
            ultimo_ack = agora
        return modo_antes, ultimo_ack
