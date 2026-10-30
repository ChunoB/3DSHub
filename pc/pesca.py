"""Pescaria: o lago aparece numa janela do PC (bom na TV, F11 = tela cheia) e cada 3DS
conectado é a vara de um pescador (até 4, cada um com uma cor).

O 3DS manda, ~30 vezes por segundo, para a porta UDP 5015:
    "3DSF" + s16 mira (-100 a 100) + u8 evento (número; muda a cada ação nova)
           + u8 tipo (1 = lançar, 2 = recolher) + u8 força (1 a 100) + u16 giro do
           molinete (graus por segundo)
O PC responde a cada pacote:
    "3DSF" + u8 estado + u8 jogador (1-4) + u8 tensão + u8 distância + u16 pontos
           + u8 mordiscando + texto UTF-8 (mensagem: peixe pego, linha quebrou...)
estado: 0 mirando, 1 linha voando, 2 boia na água, 3 fisgou (puxe!), 4 puxando,
        5 pegou, 6 perdeu

O jogo roda no PC (o 3DS só mostra): peixes nadam, se interessam pela isca, mordiscam
e fisgam. Puxando, girar o molinete traz o peixe mas estica a linha; os puxões do peixe
também. Linha a 100% arrebenta; parar de girar por muito tempo deixa o peixe fugir.
"""

import math
import random
import socket
import struct
import threading
import time

PORTA = 5015
W, H = 1280, 720                      # o mundo; a janela escala para o tamanho dela
AGUA_Y0, AGUA_Y1 = 110, 600
CORES = ["#E8483C", "#3C8CE8", "#F2B632", "#5CC85C"]
NOMES_CORES = ["Vermelho", "Azul", "Amarelo", "Verde"]

# nome, cor do corpo, cor da barbatana, comprimento (px), cm, pontos, força, raridade
ESPECIES = [
    ("Lambari",       "#E8D44C", "#C8A830", 34, 12, 10, 0.4, 10),
    ("Tilápia",       "#8CA4C0", "#5C7490", 48, 30, 20, 0.6, 8),
    ("Pacu",          "#E89448", "#B86C2C", 56, 40, 30, 0.7, 6),
    ("Tucunaré",      "#7CB85C", "#E8743C", 64, 55, 45, 0.9, 4),
    ("Dourado",       "#F4C430", "#E89418", 74, 70, 80, 1.1, 2),
    ("Bota velha",    "#5C4A3C", "#3C2E24", 40, 0, 1, 0.2, 2),
]

MIRANDO, VOANDO, NA_AGUA, FISGOU, PUXANDO, PEGOU, PERDEU = range(7)


class Peixe:
    def __init__(self):
        self.especie = random.choices(ESPECIES, weights=[e[7] for e in ESPECIES])[0]
        self.tam = random.uniform(0.75, 1.35)
        self.x = random.uniform(80, W - 80)
        self.y = random.uniform(AGUA_Y0 + 40, AGUA_Y1 - 40)
        self.ang = random.uniform(0, math.tau)
        self.vel = random.uniform(30, 60)
        self.alvo = None                 # jogador cuja isca ele quer
        self.mordisca_ate = 0.0
        self.preso = False

    @property
    def comp(self):
        return self.especie[3] * self.tam


class Jogador:
    def __init__(self, numero, origem):
        self.numero, self.origem = numero, origem
        self.x = 200 + numero * 260
        self.estado, self.mira, self.pontos = MIRANDO, 0, 0
        self.ultimo_evento, self.giro, self.visto = None, 0, time.monotonic()
        self.boia = (self.x, 640.0)
        self.voo = None                  # (de, para, início)
        self.peixe = None
        self.tensao = self.distancia = 0.0
        self.parado = 0.0                # segundos sem girar puxando
        self.proximo_puxao = 0.0
        self.mensagem, self.mensagem_ate = "", 0.0
        self.fisgou_em = 0.0

    def avisar(self, texto, segundos=3):
        self.mensagem, self.mensagem_ate = texto, time.monotonic() + segundos


class Pescaria:
    def __init__(self, config):
        self.config = config
        self.status = {"conectado": False}
        self.jogadores = {}              # origem -> Jogador
        self.peixes = [Peixe() for _ in range(9)]
        self.trava = threading.Lock()
        self.tv = None                   # thread da janela

    def iniciar(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", PORTA))
        self.sock.settimeout(1)
        threading.Thread(target=self._rede, daemon=True, name="pescaria").start()

    # ------------------------------------------------------------ rede
    def _rede(self):
        while True:
            try:
                dados, origem = self.sock.recvfrom(64)
            except (socket.timeout, OSError):
                continue
            if len(dados) < 11 or dados[:4] != b"3DSF":
                continue
            mira, evento, tipo, forca, giro = struct.unpack("<hBBBH", dados[4:11])
            with self.trava:
                j = self.jogadores.get(origem)
                if j is None:
                    livres = [n for n in range(4) if n not in {x.numero for x in self.jogadores.values()}]
                    if not livres:
                        continue
                    j = Jogador(livres[0], origem)
                    self.jogadores[origem] = j
                    j.avisar(f"Você é o pescador {NOMES_CORES[j.numero]}!", 4)
                    print(f"Pescaria: entrou o pescador {NOMES_CORES[j.numero]} ({origem[0]})")
                    self._abrir_tv()
                j.visto, j.mira, j.giro = time.monotonic(), max(-100, min(100, mira)), giro
                if j.ultimo_evento is None:
                    j.ultimo_evento = evento     # o primeiro pacote não é uma ação
                elif evento != j.ultimo_evento:
                    j.ultimo_evento = evento
                    self._acao(j, tipo, forca)
                msg = j.mensagem if time.monotonic() < j.mensagem_ate else ""
                mordisca = int(j.peixe is not None and j.estado == NA_AGUA and time.monotonic() < j.peixe.mordisca_ate)
                resposta = b"3DSF" + struct.pack("<BBBBHB", j.estado, j.numero + 1, int(j.tensao), int(j.distancia),
                                                 min(65535, j.pontos), mordisca) + msg.encode("utf-8")
            self.status["conectado"] = bool(self.jogadores)
            try:
                self.sock.sendto(resposta, origem)
            except OSError:
                pass

    def _acao(self, j, tipo, forca):
        if tipo == 1 and j.estado == MIRANDO:
            forca = max(5, min(100, forca))
            alvo_x = max(60, min(W - 60, j.x + j.mira * 4.5))
            alvo_y = AGUA_Y1 - 30 - forca / 100 * (AGUA_Y1 - AGUA_Y0 - 70)
            j.voo = ((j.x + 18, 590.0), (alvo_x, alvo_y), time.monotonic())
            j.estado = VOANDO
        elif tipo == 2 and j.estado in (NA_AGUA, VOANDO, FISGOU, PUXANDO):
            self._soltar(j)
            j.estado = MIRANDO
            j.avisar("Linha recolhida.", 2)

    def _soltar(self, j):
        if j.peixe:
            j.peixe.alvo, j.peixe.preso, j.peixe.mordisca_ate = None, False, 0.0
            j.peixe = None

    # ------------------------------------------------------------ jogo (chamado pela janela)
    def passo(self, dt):
        agora = time.monotonic()
        with self.trava:
            for origem, j in list(self.jogadores.items()):
                if agora - j.visto > 5:                  # 3DS saiu
                    self._soltar(j)
                    del self.jogadores[origem]
                    print(f"Pescaria: saiu o pescador {NOMES_CORES[j.numero]}")
                    continue
                self._passo_jogador(j, dt, agora)
            for p in self.peixes:
                self._passo_peixe(p, dt, agora)
            self.status["conectado"] = bool(self.jogadores)
            return bool(self.jogadores)

    def _passo_jogador(self, j, dt, agora):
        giro = min(j.giro / 720, 1.6)                    # 2 voltas/s = girando "cheio"
        if j.estado == VOANDO:
            (x0, y0), (x1, y1), t0 = j.voo
            t = min(1.0, (agora - t0) / 0.7)
            j.boia = (x0 + (x1 - x0) * t, y0 + (y1 - y0) * t - math.sin(t * math.pi) * 120)
            if t >= 1:
                j.estado = NA_AGUA
        elif j.estado == NA_AGUA:
            p = j.peixe
            if p and p.mordisca_ate and agora >= p.mordisca_ate:   # mordeu de verdade!
                j.estado, j.fisgou_em = FISGOU, agora
                j.avisar("FISGOU! Gire o molinete!", 1.5)
        elif j.estado == FISGOU:
            if giro > 0.25:
                j.estado = PUXANDO
                j.tensao, j.distancia, j.parado = 30.0, 100.0, 0.0
                j.peixe.preso = True
                j.proximo_puxao = agora + random.uniform(0.6, 1.4)
            elif agora - j.fisgou_em > 1.4:
                self._soltar(j)
                j.estado = PERDEU
                j.avisar("Demorou... o peixe soltou a isca.", 2.5)
        elif j.estado == PUXANDO:
            p = j.peixe
            forca = p.especie[6] * p.tam
            j.distancia -= giro * 22 * dt
            j.tensao += (giro * 55 - 30) * dt
            if agora >= j.proximo_puxao:                 # o peixe dá um puxão
                puxao = random.uniform(0.4, 1.0) * forca
                j.tensao += puxao * 22
                j.distancia = min(100, j.distancia + puxao * 6)
                j.proximo_puxao = agora + random.uniform(0.5, 1.5) / max(0.5, forca)
            j.tensao = max(0.0, j.tensao)
            j.parado = j.parado + dt if giro < 0.1 else 0.0
            # a boia vem chegando na direção do pescador
            bx, by = j.boia
            alvo = (j.x + 18, 590.0)
            k = 1 - j.distancia / 100
            j.boia = (bx + (alvo[0] - bx) * k * dt * 2, by + (alvo[1] - by) * k * dt * 2)
            if j.tensao >= 100:
                self._soltar(j)
                j.estado = PERDEU
                j.avisar("A linha arrebentou!", 2.5)
            elif j.parado > 3:
                self._soltar(j)
                j.estado = PERDEU
                j.avisar("Ficou parado... o peixe fugiu.", 2.5)
            elif j.distancia <= 0:
                nome, _, _, _, cm, pts, _, _ = p.especie
                ganho = max(1, round(pts * p.tam))
                j.pontos += ganho
                if cm:
                    j.avisar(f"Pegou um {nome} de {round(cm * p.tam)} cm! +{ganho}", 4)
                else:
                    j.avisar(f"Pegou uma {nome}... +{ganho}", 4)
                self.peixes.remove(p)
                self.peixes.append(Peixe())              # nasce outro
                j.peixe = None
                j.estado = PEGOU
        if j.estado in (PEGOU, PERDEU) and agora >= j.mensagem_ate:    # mostrou o resultado: de novo
            j.estado = MIRANDO
        if j.estado == MIRANDO:
            j.boia = (j.x + 18, 640.0)

    def _passo_peixe(self, p, dt, agora):
        if p.preso:
            dono = next((j for j in self.jogadores.values() if j.peixe is p), None)
            if dono:
                bx, by = dono.boia
                p.x += (bx - p.x) * min(1, dt * 6) + random.uniform(-2, 2)
                p.y += (by + 12 - p.y) * min(1, dt * 6) + random.uniform(-2, 2)
                p.ang = math.atan2(by - p.y, bx - p.x) + math.pi
                return
            p.preso = False
        if p.alvo is None:
            # procura uma isca por perto
            for j in self.jogadores.values():
                if j.estado == NA_AGUA and j.peixe is None:
                    bx, by = j.boia
                    if math.hypot(bx - p.x, by - p.y) < 260 and random.random() < dt * 0.35:
                        p.alvo, j.peixe = j, p
                        break
        j = p.alvo
        if j is not None and (j.peixe is not p or j.estado not in (NA_AGUA, FISGOU)):
            p.alvo, p.mordisca_ate = None, 0.0
            j = None
        if j is not None:
            bx, by = j.boia
            d = math.hypot(bx - p.x, by - p.y)
            if d > 18:
                p.ang = math.atan2(by - p.y, bx - p.x)
                p.vel = 70
            else:
                p.vel = 0
                if not p.mordisca_ate:
                    p.mordisca_ate = agora + random.uniform(1.0, 3.0)
        else:
            if random.random() < dt * 0.4:
                p.ang += random.uniform(-1.2, 1.2)
                p.vel = random.uniform(25, 60)
        p.x += math.cos(p.ang) * p.vel * dt
        p.y += math.sin(p.ang) * p.vel * dt * 0.6
        if not (60 < p.x < W - 60):
            p.ang = math.pi - p.ang
            p.x = max(60, min(W - 60, p.x))
        if not (AGUA_Y0 + 25 < p.y < AGUA_Y1 - 25):
            p.ang = -p.ang
            p.y = max(AGUA_Y0 + 25, min(AGUA_Y1 - 25, p.y))

    # ------------------------------------------------------------ janela
    def _abrir_tv(self):
        if self.tv and self.tv.is_alive():
            return
        self.tv = threading.Thread(target=self._janela, daemon=True, name="pescaria-tv")
        self.tv.start()

    def _janela(self):
        import tkinter as tk
        raiz = tk.Tk()
        raiz.title("3DS Hub - Pescaria  (F11: tela cheia)")
        raiz.geometry("1280x720")
        raiz.configure(bg="black")
        tela = tk.Canvas(raiz, bg="#1B4F72", highlightthickness=0)
        tela.pack(fill="both", expand=True)
        cheia = [False]

        def alternar(_e=None):
            cheia[0] = not cheia[0]
            raiz.attributes("-fullscreen", cheia[0])

        raiz.bind("<F11>", alternar)
        raiz.bind("<Escape>", lambda _e: (cheia.__setitem__(0, True), alternar()))
        raiz.lift()
        raiz.attributes("-topmost", True)
        raiz.after(500, lambda: raiz.attributes("-topmost", False))
        estado = {"antes": time.monotonic(), "sem_ninguem": None, "t": 0.0}

        def quadro():
            agora = time.monotonic()
            dt = min(0.1, agora - estado["antes"])
            estado["antes"] = agora
            estado["t"] += dt
            tem = self.passo(dt)
            if not tem:
                estado["sem_ninguem"] = estado["sem_ninguem"] or agora
                if agora - estado["sem_ninguem"] > 20:      # todo mundo saiu: fecha
                    raiz.destroy()
                    return
            else:
                estado["sem_ninguem"] = None
            try:
                self._desenhar(tela, estado["t"])
            except tk.TclError:
                return
            raiz.after(33, quadro)

        raiz.after(33, quadro)
        raiz.mainloop()

    def _desenhar(self, tela, t):
        cw, ch = max(1, tela.winfo_width()), max(1, tela.winfo_height())
        s = min(cw / W, ch / H)
        ox, oy = (cw - W * s) / 2, (ch - H * s) / 2

        def P(*pts):
            return [v * s + (ox if i % 2 == 0 else oy) for i, v in enumerate(pts)]

        tela.delete("all")
        tela.create_rectangle(*P(0, 0, W, AGUA_Y0), fill="#9ED4F0", width=0)            # céu
        tela.create_oval(*P(1080, 18, 1150, 88), fill="#FFE066", width=0)               # sol
        for i in range(6):                                                              # água em faixas
            y0 = AGUA_Y0 + i * (AGUA_Y1 - AGUA_Y0) / 6
            cor = ["#2E86C1", "#2874A6", "#21618C", "#1B4F72", "#154360", "#11364F"][i]
            tela.create_rectangle(*P(0, y0, W, y0 + (AGUA_Y1 - AGUA_Y0) / 6 + 1), fill=cor, width=0)
        for i in range(14):                                                             # ondinhas
            x = (i * 97 + t * 30) % W
            y = AGUA_Y0 + 20 + (i * 61) % (AGUA_Y1 - AGUA_Y0 - 40)
            tela.create_line(*P(x, y, x + 24, y), fill="#5DADE2", width=max(1, 2 * s))
        for i in range(9):                                                              # plantas no fundo
            x = 70 + i * 140
            balanço = math.sin(t * 1.5 + i) * 10
            tela.create_line(*P(x, AGUA_Y1, x + balanço, AGUA_Y1 - 60, x - balanço, AGUA_Y1 - 110),
                             fill="#1E8449", width=max(2, 6 * s), smooth=True)
        tela.create_rectangle(*P(0, AGUA_Y1, W, H), fill="#8B5A2B", width=0)            # píer
        for x in range(0, W, 80):
            tela.create_line(*P(x, AGUA_Y1, x, H), fill="#6E4520", width=max(1, 3 * s))

        with self.trava:
            for p in self.peixes:
                self._desenhar_peixe(tela, P, s, p, t)
            for j in self.jogadores.values():
                self._desenhar_jogador(tela, P, s, j, t)
            placar = sorted(self.jogadores.values(), key=lambda j: -j.pontos)
        if not placar:
            tela.create_text(*P(W / 2, H / 2), text="Abra a Pescaria no 3DS para entrar",
                             fill="white", font=("Segoe UI", int(28 * s), "bold"))
        for i, j in enumerate(placar):                                                  # placar
            y = 16 + i * 30
            tela.create_rectangle(*P(16, y, 260, y + 26), fill="#0B1E2D", outline=CORES[j.numero], width=2)
            tela.create_text(*P(28, y + 13), anchor="w", fill="white", font=("Segoe UI", int(14 * s), "bold"),
                             text=f"{NOMES_CORES[j.numero]}: {j.pontos} pontos")
        tela.create_text(*P(W - 16, H - 14), anchor="e", fill="#F0E0C0", font=("Segoe UI", int(11 * s)),
                         text="F11: tela cheia")

    def _desenhar_peixe(self, tela, P, s, p, t):
        nome, corpo, barb = p.especie[:3]
        c = p.comp
        direita = math.cos(p.ang) >= 0
        d = 1 if direita else -1
        x, y = p.x, p.y + math.sin(t * 3 + p.x) * 2
        rabo = math.sin(t * 10 + p.y) * 4
        if nome == "Bota velha":
            tela.create_rectangle(*P(x - c / 2, y - c / 3, x, y + c / 3), fill=corpo, width=0)
            tela.create_rectangle(*P(x - c / 2, y + c / 6, x + c / 2, y + c / 3), fill=corpo, width=0)
            return
        tela.create_polygon(*P(x - d * c * 0.45, y, x - d * c * 0.8, y - c * 0.28 + rabo,
                               x - d * c * 0.8, y + c * 0.28 + rabo), fill=barb, width=0)
        tela.create_oval(*P(x - c / 2, y - c * 0.25, x + c / 2, y + c * 0.25), fill=corpo, width=0)
        tela.create_polygon(*P(x - d * c * 0.1, y - c * 0.2, x + d * c * 0.1, y - c * 0.38,
                               x + d * c * 0.2, y - c * 0.2), fill=barb, width=0)
        tela.create_oval(*P(x + d * c * 0.28 - 3, y - 6, x + d * c * 0.28 + 3, y), fill="white", width=0)
        tela.create_oval(*P(x + d * c * 0.29 - 1.5, y - 4.5, x + d * c * 0.29 + 1.5, y - 1.5), fill="black", width=0)

    def _desenhar_jogador(self, tela, P, s, j, t):
        cor = CORES[j.numero]
        x, y = j.x, AGUA_Y1 + 10
        tela.create_oval(*P(x - 14, y - 4, x + 14, y + 24), fill=cor, width=0)                 # cabeça
        tela.create_rectangle(*P(x - 18, y + 22, x + 18, y + 80), fill=cor, width=0)          # corpo
        tela.create_arc(*P(x - 20, y - 14, x + 20, y + 14), start=0, extent=180, fill="#F2D16B", width=0)  # chapéu
        ponta = (x + 40, y - 50)
        tela.create_line(*P(x + 10, y + 40, *ponta), fill="#5C3A1E", width=max(2, 4 * s))   # vara
        if j.estado == MIRANDO:                                                           # mira
            mx = max(60, min(W - 60, j.x + j.mira * 4.5))
            tela.create_line(*P(mx, AGUA_Y0 + 40, mx, AGUA_Y1 - 20), fill=cor, dash=(6, 6), width=max(1, 2 * s))
            tela.create_polygon(*P(mx - 8, AGUA_Y1 - 30, mx + 8, AGUA_Y1 - 30, mx, AGUA_Y1 - 18), fill=cor)
        else:
            bx, by = j.boia
            mordisca = j.peixe is not None and j.estado == NA_AGUA and time.monotonic() < j.peixe.mordisca_ate
            if mordisca or j.estado in (FISGOU, PUXANDO):
                by += math.sin(t * 25) * 4 + (6 if j.estado != NA_AGUA else 0)
            tela.create_line(*P(*ponta, bx, by), fill="#EEEEEE", width=max(1, s))
            tela.create_oval(*P(bx - 7, by - 7, bx + 7, by + 7), fill="white", outline=cor, width=max(1, 2 * s))
            tela.create_arc(*P(bx - 7, by - 7, bx + 7, by + 7), start=0, extent=180, fill=cor, width=0)
            if j.estado == FISGOU:
                tela.create_text(*P(bx, by - 26), text="!", fill="#FFE066", font=("Segoe UI", int(28 * s), "bold"))
        if time.monotonic() < j.mensagem_ate and j.mensagem:
            tela.create_text(*P(x, AGUA_Y1 - 14), text=j.mensagem, fill="white",
                             font=("Segoe UI", int(13 * s), "bold"))
