"""Som: o áudio do PC (o que sai nos alto-falantes) tocando no 3DS.

Enquanto quer som, o 3DS manda "3DSU" para a porta UDP 5013 a cada segundo. O PC
grava o que está tocando (o "loopback" do Windows), compacta em IMA ADPCM (4 bits
por amostra, 22050 Hz, mono: uns 11 KB/s) e manda em pacotes de 512 amostras.
Sem "3DSU" por 3 segundos, para de gravar.

Pacote: "3DSS" + u32 seq + s16 previsão + u8 índice + u8 0 + 256 bytes ADPCM.
A previsão e o índice são o estado do compactador no começo do pacote, então cada
pacote se decodifica sozinho (um pacote perdido no Wi-Fi não estraga os outros).
Em cada byte, a primeira amostra fica nos 4 bits de cima (como no audioop).
"""

import ctypes
import socket
import struct
import threading
import time
import warnings

try:
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", DeprecationWarning)
        import audioop           # Python 3.12; no 3.13+ vem do pacote audioop-lts
    import numpy as np
    import soundcard
except Exception as e:           # sem as bibliotecas: o resto do Hub funciona sem som
    soundcard = None
    _erro_som = repr(e)

# "data discontinuity": o Windows avisa de cada falhinha na gravação (inofensivo,
# vira um estalo no máximo) e encheria o hub.log
warnings.filterwarnings("ignore", message="data discontinuity")

PORTA = 5013
TAXA = 22050
AMOSTRAS = 512                   # por pacote (23 ms)


class Som:
    def __init__(self, config):
        self.config = config
        self.status = {"conectado": False}
        self.destino, self.ultimo_oi = None, 0.0

    def iniciar(self):
        if soundcard is None:
            raise RuntimeError(f"bibliotecas de som faltando? {_erro_som}")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", PORTA))
        self.sock.settimeout(1)
        threading.Thread(target=self._escutar, daemon=True, name="som-escuta").start()
        threading.Thread(target=self._gravar, daemon=True, name="som").start()

    def _ativo(self):
        return self.destino is not None and time.monotonic() - self.ultimo_oi < 3

    def _escutar(self):
        while True:
            try:
                dados, origem = self.sock.recvfrom(64)
            except (socket.timeout, OSError):
                continue
            if dados[:4] == b"3DSU":
                if not self._ativo():
                    print(f"Som: 3DS quer o som ({origem[0]})")
                self.destino, self.ultimo_oi = origem, time.monotonic()

    def _gravar(self):
        # O som do Windows (WASAPI) usa COM, que cada thread precisa preparar. Sem isto,
        # dentro do Hub (onde o ícone e a captura já usam COM de outro jeito) a gravação
        # falhava com o erro 0x800401f0 ("COM não iniciado").
        ctypes.windll.ole32.CoInitializeEx(None, 0)     # 0 = COINIT_MULTITHREADED
        while True:
            if not self._ativo():
                self.status["conectado"] = False
                time.sleep(0.2)
                continue
            self.status["conectado"] = True
            try:
                # a saída padrão de agora (fone, caixa...): se trocar, grava da nova na próxima volta
                alto = soundcard.default_speaker()
                entrada = soundcard.get_microphone(id=str(alto.name), include_loopback=True)
                with entrada.recorder(samplerate=TAXA, channels=1, blocksize=AMOSTRAS) as gravador:
                    estado, seq = None, 0
                    while self._ativo():
                        bloco = gravador.record(numframes=AMOSTRAS)
                        pcm = (np.clip(bloco[:, 0], -1, 1) * 32767).astype("<i2").tobytes()
                        previsao, indice = estado or (0, 0)
                        with warnings.catch_warnings():
                            warnings.simplefilter("ignore", DeprecationWarning)
                            comprimido, estado = audioop.lin2adpcm(pcm, 2, estado)
                        cab = b"3DSS" + struct.pack("<IhBB", seq, previsao, indice, 0)
                        self.sock.sendto(cab + comprimido, self.destino)
                        seq += 1
            except Exception as e:
                print("Som: erro ao gravar o som do PC", repr(e))
                time.sleep(2)
