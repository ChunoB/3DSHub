"""Ajustes e atualização: o 3DS baixa a versão mais nova do app pelo Wi-Fi e
muda as configurações do controle sem abrir o config.json.

Protocolo (TCP, porta 5010; o 3DS pede e o PC responde):
    "I <tipo>\\n"        -> "3DSI" + u32 tamanho + u32 crc32   (tipo = 3dsx ou cia; 0/0 = não tem)
    "B <tipo>\\n"        -> "3DSB" + u32 tamanho + o arquivo
    "C\\n"               -> "3DSC" + u32 tamanho + "chave=valor\\n" das configurações do controle
    "W <chave> <valor>\\n" -> "3DSW" + u32 (1 = gravou, 0 = recusou)
    "R\\n"               -> "3DSR" + u32 tamanho + "token\\ntopico\\n"   (liga o acesso pela internet;
                            só aceito pela rede de casa, nunca por quem já entrou pela internet)
"""

import copy
import os
import socket
import struct
import threading
import time
import zlib

import remoto
from config import PASTA

PORTA = 5010

# só estas configurações podem ser mudadas pelo 3DS (e com estes limites)
CHAVES = {
    "giro_pixels_por_grau":     (float, 1, 80),
    "giro_analogico_graus":     (float, 20, 600),
    "giro_zona_morta":          (float, 0, 15),
    "giro_inverter_horizontal": (bool, 0, 1),
    "giro_inverter_vertical":   (bool, 0, 1),
    "sensibilidade_toque":      (float, 0.25, 10),
}

ARQUIVOS = {   # onde procurar o app (o mais novo vence): pasta do projeto e pasta do pacote
    "3dsx": ["../3ds/3dshub.3dsx", "../3dshub.3dsx"],
    "cia":  ["../3ds/3dshub.cia", "../3dshub.cia"],
}


def arquivo_app(tipo):
    caminhos = [os.path.normpath(os.path.join(PASTA, c)) for c in ARQUIVOS.get(tipo, [])]
    existentes = [c for c in caminhos if os.path.isfile(c)]
    return max(existentes, key=os.path.getmtime) if existentes else None


_crc_cache = {}


def info_app(tipo):
    """(tamanho, crc32) do arquivo, guardando o crc enquanto o arquivo não muda."""
    caminho = arquivo_app(tipo)
    if not caminho:
        return None, 0, 0
    chave = (caminho, os.path.getmtime(caminho), os.path.getsize(caminho))
    if _crc_cache.get(tipo, (None,))[0] != chave:
        with open(caminho, "rb") as f:
            _crc_cache[tipo] = (chave, zlib.crc32(f.read()) & 0xFFFFFFFF)
    return caminho, chave[2], _crc_cache[tipo][1]


def formatar(valor):
    if isinstance(valor, bool):
        return "1" if valor else "0"
    return f"{float(valor):g}"


class Ajustes:
    def __init__(self, config):
        self.config = config
        self.status = {"conectado": False}

    def iniciar(self):
        self.servidor = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.servidor.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        self.servidor.bind(("0.0.0.0", PORTA))
        self.servidor.listen(1)
        threading.Thread(target=self._aceitar, daemon=True, name="ajustes").start()

    def _aceitar(self):
        while True:
            try:
                conn, origem = self.servidor.accept()
            except OSError:
                time.sleep(1)
                continue
            print(f"Ajustes: 3DS conectado ({origem[0]})")
            self.status["conectado"] = True
            try:
                self._atender(conn, origem[0])
            except Exception as e:
                print("Ajustes: desconectou", repr(e))
            finally:
                conn.close()
                self.status["conectado"] = False

    def _gravar(self, chave, texto):
        if chave not in CHAVES:
            return False
        tipo, minimo, maximo = CHAVES[chave]
        try:
            if tipo is bool:
                valor = texto.strip() in ("1", "true", "sim")
            else:
                valor = max(minimo, min(maximo, float(texto)))
        except ValueError:
            return False
        dados = copy.deepcopy(self.config._dados)
        dados["controle"][chave] = valor
        self.config.salvar(dados)
        print(f"Ajustes: {chave} = {formatar(valor)}")
        return True

    def _atender(self, conn, ip):
        conn.settimeout(60)
        arquivo = conn.makefile("rb")
        while True:
            linha = arquivo.readline()
            if not linha:
                return
            partes = linha.decode("utf-8", "ignore").split()
            if not partes:
                continue
            cmd = partes[0]

            if cmd == "I" and len(partes) == 2:
                _, tamanho, crc = info_app(partes[1])
                conn.sendall(b"3DSI" + struct.pack("<II", tamanho, crc))

            elif cmd == "B" and len(partes) == 2:
                caminho, tamanho, _ = info_app(partes[1])
                if not caminho:
                    conn.sendall(b"3DSB" + struct.pack("<I", 0))
                    continue
                with open(caminho, "rb") as f:
                    dados = f.read()
                print(f"Ajustes: enviando {os.path.basename(caminho)} ({len(dados) // 1024} KB)")
                conn.sendall(b"3DSB" + struct.pack("<I", len(dados)) + dados)

            elif cmd == "C":
                ctl = self.config["controle"]
                texto = "".join(f"{k}={formatar(ctl.get(k, 0))}\n" for k in CHAVES).encode()
                conn.sendall(b"3DSC" + struct.pack("<I", len(texto)) + texto)

            elif cmd == "W" and len(partes) == 3:
                ok = self._gravar(partes[1], partes[2])
                conn.sendall(b"3DSW" + struct.pack("<I", 1 if ok else 0))

            elif cmd == "R":
                # parear só pela rede de casa: quem chega pelo portão da internet vem de 127.0.0.1
                servico = remoto.obter()
                if ip.startswith("127.") or not servico:
                    conn.sendall(b"3DSR" + struct.pack("<I", 0))
                    continue
                token, topico = servico.parear()
                texto = f"{token}\n{topico}\n".encode()
                print("Ajustes: 3DS pareado para acesso pela internet")
                conn.sendall(b"3DSR" + struct.pack("<I", len(texto)) + texto)
