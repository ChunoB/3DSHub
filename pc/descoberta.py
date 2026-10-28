"""Descoberta: o 3DS pergunta para a rede inteira "tem algum 3DS Hub aí?"
(broadcast UDP na porta 5009) e o PC responde com o nome dele.

O 3DS usa o endereço de quem respondeu, então ninguém precisa digitar o IP,
nem quando a rede ou o hotspot mudam.

    3DS -> "3DSD"             (pergunta, para 255.255.255.255:5009)
    PC  -> "3DSD" + nome      (resposta, direto para o 3DS)
"""

import socket
import threading

PORTA = 5009


class Descoberta:
    def __init__(self, config):
        self.config = config
        self.status = {}

    def iniciar(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", PORTA))
        threading.Thread(target=self._loop, daemon=True, name="descoberta").start()

    def _loop(self):
        nome = socket.gethostname()[:30].encode("utf-8", "ignore")
        while True:
            try:
                dados, origem = self.sock.recvfrom(64)
            except OSError:
                continue
            # só a pergunta (4 bytes); respostas de outros PCs também começam com 3DSD
            if dados == b"3DSD":
                try:
                    self.sock.sendto(b"3DSD" + nome, origem)
                except OSError:
                    pass
