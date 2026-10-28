"""Segunda Tela: manda as estatísticas do PC (com histórico para os gráficos) e
executa os comandos de música, volume e atalhos que o 3DS manda."""

import asyncio
import collections
import ctypes
import os
import socket
import threading
import time
import unicodedata

import psutil
import win32pdh
from pycaw.pycaw import AudioUtilities
from pynput.keyboard import Key
from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as GerenciadorMidia,
)

from util import combinacao, teclado

PORTA = 5006
INTERVALO_S = 1.0
HISTORICO = 120          # amostras (2 minutos)
PASSO_VOLUME = 0.05

CMD_PLAY, CMD_PROXIMA, CMD_ANTERIOR = 1, 2, 3
CMD_VOL_MAIS, CMD_VOL_MENOS, CMD_MUDO = 4, 5, 6
CMD_ATALHO = 100         # 100 + índice do atalho

DIAS = ["seg", "ter", "qua", "qui", "sex", "sab", "dom"]


def texto(t, maximo=48):
    """Texto para o 3DS: mantém os acentos (a interface nova mostra), tira quebras
    de linha, caracteres de controle e '|' (separa os atalhos), e corta por letras."""
    t = unicodedata.normalize("NFC", t or "").replace("|", "/")
    t = "".join(c if c.isprintable() else " " for c in t)
    return t.strip()[:maximo]


def codificar(valores, maximo):
    """Histórico em 1 caractere por amostra: 'A' = 0 ... 's' = máximo (51 níveis)."""
    return "".join(chr(65 + round(max(0, min(maximo, v)) / maximo * 50)) for v in valores)


# ---------------------------------------------------------------- GPU
class GPU:
    def __init__(self):
        self.query = None
        self.contadores = []
        self.montado_em = 0.0

    def _montar(self):
        if self.query:
            win32pdh.CloseQuery(self.query)
        self.query = win32pdh.OpenQuery()
        caminho = win32pdh.MakeCounterPath(
            (None, "GPU Engine", "*engtype_3D", None, -1, "Utilization Percentage"))
        self.contadores = [win32pdh.AddCounter(self.query, c)
                           for c in win32pdh.ExpandCounterPath(caminho)]
        win32pdh.CollectQueryData(self.query)
        self.montado_em = time.monotonic()

    def ler(self):
        try:
            if not self.query or time.monotonic() - self.montado_em > 15:
                self._montar()
                return 0
            win32pdh.CollectQueryData(self.query)
            total = 0.0
            for c in self.contadores:
                try:
                    total += win32pdh.GetFormattedCounterValue(c, win32pdh.PDH_FMT_DOUBLE)[1]
                except Exception:
                    pass
            return min(100, round(total))
        except Exception:
            return -1


# ---------------------------------------------------------------- mídia e volume
class Midia:
    """Lê a música tocando. Reaproveita o loop assíncrono e o gerenciador de
    mídia do Windows em vez de criar os dois de novo a cada segundo."""

    def __init__(self):
        self.loop = asyncio.new_event_loop()
        self.gerenciador = None

    async def _ler(self):
        if self.gerenciador is None:
            self.gerenciador = await GerenciadorMidia.request_async()
        sessao = self.gerenciador.get_current_session()
        if not sessao:
            return "", "", 0
        props = await sessao.try_get_media_properties_async()
        return props.title, props.artist, int(sessao.get_playback_info().playback_status == 4)

    def ler(self):
        try:
            return self.loop.run_until_complete(self._ler())
        except Exception:
            self.gerenciador = None      # pede de novo na próxima vez
            return "", "", 0


def volume():
    return AudioUtilities.GetSpeakers().EndpointVolume


def ajustar_volume(delta):
    v = volume()
    v.SetMute(0, None)
    v.SetMasterVolumeLevelScalar(max(0.0, min(1.0, v.GetMasterVolumeLevelScalar() + delta)), None)


def alternar_mudo():
    v = volume()
    v.SetMute(0 if v.GetMute() else 1, None)


# ---------------------------------------------------------------- estatísticas
class Estatisticas:
    def __init__(self, config):
        self.config = config
        self.gpu = GPU()
        self.midia = Midia()
        self.rede_antes = psutil.net_io_counters()
        self.t_antes = time.monotonic()
        psutil.cpu_percent()
        self.hist = {k: collections.deque([0] * HISTORICO, maxlen=HISTORICO)
                     for k in ("cpu", "gpu", "ram", "down", "up")}
        self.texto = b""
        self.atualizar()

    def atualizar(self):
        agora = time.monotonic()
        rede = psutil.net_io_counters()
        dt = max(agora - self.t_antes, 0.001)
        down = (rede.bytes_recv - self.rede_antes.bytes_recv) / dt / 1024
        up = (rede.bytes_sent - self.rede_antes.bytes_sent) / dt / 1024
        self.rede_antes, self.t_antes = rede, agora

        cpu = round(psutil.cpu_percent())
        gpu = self.gpu.ler()
        ram = psutil.virtual_memory()
        disco = psutil.disk_usage("C:\\")
        bat = psutil.sensors_battery()
        titulo, artista, tocando = self.midia.ler()
        try:
            v = volume()
            vol, mudo = round(v.GetMasterVolumeLevelScalar() * 100), v.GetMute()
        except Exception:
            vol, mudo = -1, 0

        for k, val in (("cpu", cpu), ("gpu", max(gpu, 0)), ("ram", ram.percent),
                       ("down", down), ("up", up)):
            self.hist[k].append(val)
        # escala da rede: o maior valor da janela (mínimo 100 KB/s)
        rede_max = max(100, max(self.hist["down"]), max(self.hist["up"]))

        hora = time.localtime()
        atalhos = "|".join(texto(str(a.get("nome", "?")), 10) for a in self.config["atalhos"][:9])
        campos = {
            "hora": time.strftime("%H:%M", hora),
            "data": f"{DIAS[hora.tm_wday]} {time.strftime('%d/%m', hora)}",
            "cpu": cpu, "gpu": gpu, "ram": round(ram.percent),
            "ramu": f"{ram.used / 2**30:.1f}", "ramt": f"{ram.total / 2**30:.1f}",
            "disco": round(disco.percent),
            "down": round(down), "up": round(up),
            "bat": round(bat.percent) if bat else -1,
            "carg": int(bat.power_plugged) if bat else 0,
            "vol": vol, "mudo": int(mudo),
            "titulo": texto(titulo), "artista": texto(artista), "tocando": tocando,
            "hcpu": codificar(self.hist["cpu"], 100),
            "hgpu": codificar(self.hist["gpu"], 100),
            "hram": codificar(self.hist["ram"], 100),
            "hdown": codificar(self.hist["down"], rede_max),
            "hup": codificar(self.hist["up"], rede_max),
            "redemax": round(rede_max),
            "atalhos": atalhos,
        }
        self.texto = ("3DST\n" + "".join(f"{k}={v}\n" for k, v in campos.items())).encode()


# ---------------------------------------------------------------- servidor
class SegundaTela:
    def __init__(self, config):
        self.config = config
        self.status = {"conectado": False}

    def iniciar(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", PORTA))
        self.sock.settimeout(0.1)
        threading.Thread(target=self._loop, daemon=True, name="tela").start()

    def _enviar(self, dados, origem):
        """Envio que nunca derruba o serviço: se a rede mudar ou cair (troca de
        Wi-Fi, hotspot desligado), esse envio se perde e o próximo tenta de novo."""
        try:
            self.sock.sendto(dados, origem)
        except OSError as e:
            print("Segunda Tela: nao consegui responder ao 3DS:", e)

    def _atalho(self, i):
        atalhos = self.config["atalhos"]
        if i >= len(atalhos):
            return
        a = atalhos[i]
        tipo, valor = a.get("tipo"), a.get("valor", "")
        print(f"Atalho: {a.get('nome')}")
        if tipo == "teclas":
            combinacao(valor)
        elif tipo == "abrir":
            os.startfile(valor)
        elif tipo == "travar":
            ctypes.windll.user32.LockWorkStation()

    def _executar(self, cmd):
        try:
            if cmd == CMD_PLAY: teclado.tap(Key.media_play_pause)
            elif cmd == CMD_PROXIMA: teclado.tap(Key.media_next)
            elif cmd == CMD_ANTERIOR: teclado.tap(Key.media_previous)
            elif cmd == CMD_VOL_MAIS: ajustar_volume(+PASSO_VOLUME)
            elif cmd == CMD_VOL_MENOS: ajustar_volume(-PASSO_VOLUME)
            elif cmd == CMD_MUDO: alternar_mudo()
            elif cmd >= CMD_ATALHO: self._atalho(cmd - CMD_ATALHO)
        except Exception as e:
            print("Erro no comando", cmd, e)

    def _loop(self):
        # Obs.: não chamar CoInitializeEx aqui. O comtypes (usado pelo pycaw)
        # já cuida do COM, e iniciar outro modo nesta thread trava o volume.
        est = Estatisticas(self.config)
        ultima = time.monotonic()
        visto = 0.0

        while True:
            if time.monotonic() - ultima >= INTERVALO_S:
                try:
                    est.atualizar()
                except Exception as e:
                    print("Erro nas estatisticas:", e)
                ultima = time.monotonic()

            try:
                dados, origem = self.sock.recvfrom(64)
            except socket.timeout:
                if visto and time.monotonic() - visto > 3:
                    print("Segunda Tela: 3DS desconectou.")
                    visto = 0.0
                    self.status["conectado"] = False
                continue
            except OSError:
                continue

            if dados[:4] == b"3DSP":
                # "ping" do menu do 3DS (só quer saber se o Hub está no ar):
                # responde, mas não conta como Segunda Tela conectada
                self._enviar(b"3DSP", origem)
                continue
            if dados[:4] not in (b"3DSH", b"3DSC"):
                continue
            if not visto:
                print(f"Segunda Tela: 3DS conectado ({origem[0]})")
                self.status["conectado"] = True
            visto = time.monotonic()

            if dados[:4] == b"3DSC" and len(dados) >= 5:
                self._executar(dados[4])
                if dados[4] < CMD_ATALHO:
                    est.atualizar()          # mostra o volume novo na hora
                    ultima = time.monotonic()
            self._enviar(est.texto, origem)
