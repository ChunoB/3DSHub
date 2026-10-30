"""Configuração do 3DS Hub: lê o config.json e recarrega sozinho quando ele muda."""

import copy
import json
import os
import threading
import time

PASTA = os.path.dirname(os.path.abspath(__file__))
ARQUIVO = os.path.join(PASTA, "config.json")

PADRAO = {
    "iniciar_com_windows": True,

    "controle": {
        "sensibilidade_toque": 2.5,       # pixels do PC por pixel da tela do 3DS
        "giro_pixels_por_grau": 15,        # giroscópio -> mouse
        "giro_analogico_graus": 150,      # giroscópio -> analógico: graus/s para o máximo
        "giro_zona_morta": 2,             # graus/s ignorados (tremida da mão)
        "giro_inverter_horizontal": False,
        "giro_inverter_vertical": False,
        "camera_pixels_por_quadro": 15,   # modo teclado: analógico direito -> mouse (no máximo)
        "analogico_direito_sensibilidade": 1.0,  # C-stick / analógico de toque: multiplica o valor
        "giro_pausar_no_toque": True,     # dedo na tela pausa o giroscópio ("levantar o mouse")
        "giro_l_r_como_cliques": True,    # modo controle + giroscópio no mouse: L/R = cliques
    },

    # O primeiro perfil é o padrão. Os outros entram sozinhos quando o
    # programa da lista está em primeiro plano (nome do .exe).
    #   modo: "controle" (Xbox 360 virtual) ou "teclado"
    #   giroscopio: "mouse" ou "analogico_direito"
    #   toque (só modo controle, app novo do 3DS): "mouse" (touchpad), "gatilhos"
    #     (metade esquerda/direita = LT/RT) ou "controle" (LT RT LB RB + analógico direito)
    #   analogico (só modo teclado): "wasd", "setas", "mouse" ou "nada"
    #   teclas (só modo teclado): botão do 3DS -> tecla
    #     teclas: uma letra, space, shift, ctrl, alt, tab, esc, enter,
    #     up/down/left/right, f1..f12, mouse_esquerdo, mouse_direito, mouse_meio
    "perfis": [
        {
            "nome": "Padrao",
            "processos": [],
            "modo": "controle",
            "l_r_como_gatilhos": False,
            "giroscopio": "mouse",
        },
        {
            "nome": "Minecraft",
            "processos": ["javaw.exe"],
            "modo": "teclado",
            "giroscopio": "mouse",
            "analogico": "wasd",
            "teclas": {
                "A": "space", "B": "shift", "X": "e", "Y": "q",
                "L": "mouse_direito", "R": "mouse_esquerdo",
                "START": "esc", "SELECT": "tab",
                "CIMA": "f5", "BAIXO": "ctrl", "ESQUERDA": "1", "DIREITA": "2",
            },
            # o que aparece na tela de cima do 3DS para cada botão (opcional)
            "legenda": {
                "A": "Pular", "B": "Agachar", "X": "Inventário", "Y": "Soltar item",
                "L": "Usar / colocar", "R": "Atacar / quebrar",
                "START": "Menu", "SELECT": "Jogadores", "CIMA": "Câmera (F5)", "BAIXO": "Correr",
                "ESQUERDA": "Item 1", "DIREITA": "Item 2", "CIRCLE": "Andar (WASD)", "CSTICK": "Olhar",
            },
        },
        {
            # LoL é mouse + teclado: ABXY = habilidades (Y X A B = Q W E R, na ordem
            # em que ficam no teclado), R = clique direito (andar/atacar), L = clique
            # esquerdo, cruz = feitiços/voltar/sentinela, circle pad = cursor
            "nome": "League of Legends",
            "processos": ["League of Legends.exe"],
            "modo": "teclado",
            "mouse": "cursor",          # o LoL ignora o mouse simulado: teleporta o cursor
            "giroscopio": "mouse",
            "analogico": "mouse",
            "teclas": {
                "Y": "q", "X": "w", "A": "e", "B": "r",
                "R": "mouse_direito", "L": "mouse_esquerdo",
                "CIMA": "d", "DIREITA": "f", "BAIXO": "b", "ESQUERDA": "4",
                "START": "esc", "SELECT": "tab",
                "ZL": "space", "ZR": "ctrl",
            },
            "legenda": {
                "Y": "Q", "X": "W", "A": "E", "B": "R (ultimate)",
                "R": "Andar / atacar", "L": "Selecionar / mirar",
                "CIMA": "D (feitiço 1)", "DIREITA": "F (feitiço 2)", "BAIXO": "B (voltar à base)",
                "ESQUERDA": "4 (sentinela)", "START": "Esc (menu)", "SELECT": "Tab (placar)",
                "ZL": "Espaço (câmera)", "ZR": "Ctrl (subir skill)", "CIRCLE": "Cursor",
            },
        },
        {
            # Genshin no controle: L/R = LB/RB; tocar na metade esquerda/direita da
            # tela de baixo = LT/RT (3DS sem ZL/ZR); giroscópio mexe a câmera
            "nome": "Genshin Impact",
            "processos": ["GenshinImpact.exe", "YuanShen.exe"],
            "modo": "controle",
            "l_r_como_gatilhos": False,
            "toque": "gatilhos",
            "giroscopio": "analogico_direito",
        },
        {
            # It Takes Two: L/R = LB/RB; a tela de baixo do 3DS vira LT RT LB RB +
            # analógico direito; giroscópio também mexe a câmera
            "nome": "It Takes Two",
            "processos": ["ItTakesTwo.exe"],
            "modo": "controle",
            "l_r_como_gatilhos": False,
            "toque": "controle",
            "giroscopio": "analogico_direito",
        },
        {
            # controlar outro PC pelo Parsec: o controle de Xbox daqui vai junto pelo
            # Parsec (lá aparece como um Xbox 360); o giroscópio vai no analógico
            # direito para também passar pelo controle
            "nome": "Parsec",
            "processos": ["parsecd.exe"],
            "modo": "controle",
            "l_r_como_gatilhos": False,
            "giroscopio": "analogico_direito",
        },
    ],

    # Teclado usado quando você força o modo teclado (SELECT+A no 3DS)
    # num perfil que é de controle.
    "teclado_padrao": {
        "analogico": "setas",
        "teclas": {
            "A": "space", "B": "shift", "X": "e", "Y": "r",
            "L": "q", "R": "f", "START": "esc", "SELECT": "tab",
            "CIMA": "w", "BAIXO": "s", "ESQUERDA": "a", "DIREITA": "d",
        },
    },

    # Página de atalhos da Segunda Tela (até 9).
    #   tipo "teclas": combinação, ex "ctrl+shift+m" (win = tecla Windows)
    #   tipo "abrir":  programa, pasta, site ou link (ex "calc", "https://...")
    #   tipo "travar": trava o PC
    "atalhos": [
        {"nome": "Mutar mic", "tipo": "teclas", "valor": "ctrl+shift+m"},
        {"nome": "Ensurdecer", "tipo": "teclas", "valor": "ctrl+shift+d"},
        {"nome": "Travar PC", "tipo": "travar"},
        {"nome": "Print", "tipo": "teclas", "valor": "win+shift+s"},
        {"nome": "Area trab.", "tipo": "teclas", "valor": "win+d"},
        {"nome": "Tarefas", "tipo": "abrir", "valor": "taskmgr"},
        {"nome": "Navegador", "tipo": "abrir", "valor": "https://www.google.com"},
        {"nome": "Arquivos", "tipo": "abrir", "valor": "explorer"},
        {"nome": "Calculad.", "tipo": "abrir", "valor": "calc"},
    ],

    "camera": {
        "porta_http": 8090,
        "modo_3d": "anaglifo",     # "anaglifo" (óculos vermelho/azul) ou "lado_a_lado"
    },

    "espelho": {
        "qualidade_jpeg": 70,
    },

    # Steam: o jogo na tela de cima do 3DS. Qualidade menor = mais quadros por segundo.
    "steam": {
        "qualidade_jpeg": 60,
        # modo Blocos: quanto um bloco precisa mudar (diferença média, 0 a 255) para
        # ser reenviado. Maior = ignora fundos que mexem pouco (e manda menos).
        "limiar_blocos": 3,
        # som do PC no 3DS: o Hub aumenta sozinho o som baixo, até este tanto de vezes
        # (1 = como sai do PC). Um limitador suave evita estourar quando o som é alto.
        "volume_som": 8.0,
    },
}


def _mesclar(base, novo):
    """Completa o que faltar no config do usuário com os valores padrão."""
    saida = copy.deepcopy(base)
    for k, v in novo.items():
        if isinstance(v, dict) and isinstance(saida.get(k), dict):
            saida[k] = _mesclar(saida[k], v)
        else:
            saida[k] = v
    return saida


class Config:
    def __init__(self):
        self._dados = copy.deepcopy(PADRAO)
        self._mtime = 0.0
        self.erro = None
        if not os.path.exists(ARQUIVO):
            self.salvar(PADRAO)
        self.recarregar()
        threading.Thread(target=self._vigiar, daemon=True).start()

    def __getitem__(self, chave):
        return self._dados[chave]

    def get(self, chave, padrao=None):
        return self._dados.get(chave, padrao)

    def salvar(self, dados=None):
        if dados is not None:
            self._dados = copy.deepcopy(dados)
        with open(ARQUIVO, "w", encoding="utf-8") as f:
            json.dump(self._dados, f, indent=2, ensure_ascii=False)
        self._mtime = os.path.getmtime(ARQUIVO)

    def recarregar(self):
        try:
            with open(ARQUIVO, encoding="utf-8") as f:
                self._dados = _mesclar(PADRAO, json.load(f))
            self.erro = None
        except (OSError, ValueError) as e:
            # config com erro de digitação: continua com o último que funcionou
            self.erro = str(e)
            print("Erro no config.json:", e)
        self._mtime = os.path.getmtime(ARQUIVO) if os.path.exists(ARQUIVO) else 0

    def _vigiar(self):
        while True:
            time.sleep(2)
            try:
                if os.path.getmtime(ARQUIVO) != self._mtime:
                    self.recarregar()
                    print("config.json recarregado")
            except OSError:
                pass
