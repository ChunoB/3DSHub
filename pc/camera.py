"""Câmera: recebe as fotos JPEG do 3DS por TCP e serve como vídeo MJPEG para o OBS.

No OBS: Fonte > Navegador > URL http://127.0.0.1:8090/obs  (largura 400, altura 240;
no modo 3D lado a lado use largura 800).
"""

import io
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from PIL import Image, ImageChops

PORTA = 5007
CABECALHO = "<4sBBHI"            # "3DSF", olho (0 esq / 1 dir), total de olhos, reservado, tamanho
TAM_CABECALHO = struct.calcsize(CABECALHO)

PAGINA_OBS = b"""<!doctype html><html><head><meta charset="utf-8"><style>
html,body{margin:0;height:100%;background:#000;overflow:hidden}
img{width:100%;height:100%;object-fit:contain}
</style></head><body><img src="/camera.mjpg"></body></html>"""


def receber_tudo(sock, n):
    dados = bytearray()
    while len(dados) < n:
        parte = sock.recv(n - len(dados))
        if not parte:
            raise ConnectionError("3DS desconectou")
        dados += parte
    return bytes(dados)


class Camera:
    def __init__(self, config):
        self.config = config
        self.status = {"conectado": False, "fps": 0.0}
        self.olhos = [None, None]
        self.quadro = None              # JPEG final que vai para o OBS
        self.novo_quadro = threading.Condition()
        self.numero = 0

    def url_obs(self):
        return f"http://127.0.0.1:{self.config['camera']['porta_http']}/obs"

    def iniciar(self):
        self.servidor_tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.servidor_tcp.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        self.servidor_tcp.bind(("0.0.0.0", PORTA))
        self.servidor_tcp.listen(1)
        threading.Thread(target=self._aceitar, daemon=True, name="camera").start()

        camera = self

        class Http(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                if self.path in ("/", "/obs"):
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                    self.end_headers()
                    self.wfile.write(PAGINA_OBS)
                elif self.path.startswith("/camera.mjpg"):
                    camera._transmitir(self)
                elif self.path.startswith("/foto.jpg") and camera.quadro:
                    self.send_response(200)
                    self.send_header("Content-Type", "image/jpeg")
                    self.end_headers()
                    self.wfile.write(camera.quadro)
                else:
                    self.send_error(404)

        # só no próprio PC (127.0.0.1): o OBS roda aqui, ninguém da rede precisa ver
        self.http = ThreadingHTTPServer(("127.0.0.1", self.config["camera"]["porta_http"]), Http)
        self.http.daemon_threads = True
        threading.Thread(target=self.http.serve_forever, daemon=True, name="camera-http").start()

    def _transmitir(self, req):
        req.send_response(200)
        req.send_header("Content-Type", "multipart/x-mixed-replace; boundary=quadro")
        req.send_header("Cache-Control", "no-cache")
        req.end_headers()
        visto = -1
        try:
            while True:
                with self.novo_quadro:
                    self.novo_quadro.wait_for(lambda: self.numero != visto, timeout=5)
                if self.quadro is None or self.numero == visto:
                    continue
                visto = self.numero
                q = self.quadro
                req.wfile.write(b"--quadro\r\nContent-Type: image/jpeg\r\n"
                                + f"Content-Length: {len(q)}\r\n\r\n".encode() + q + b"\r\n")
        except (ConnectionError, OSError):
            pass   # o OBS fechou a fonte

    def _compor(self, total):
        """Junta os dois olhos conforme o modo 3D escolhido."""
        if total < 2 or self.olhos[1] is None:
            return self.olhos[0]
        esq = Image.open(io.BytesIO(self.olhos[0])).convert("RGB")
        dir_ = Image.open(io.BytesIO(self.olhos[1])).convert("RGB")
        if self.config["camera"].get("modo_3d") == "lado_a_lado":
            img = Image.new("RGB", (esq.width + dir_.width, esq.height))
            img.paste(esq, (0, 0))
            img.paste(dir_, (esq.width, 0))
        else:   # anaglifo vermelho/ciano: vermelho do olho esquerdo, verde e azul do direito
            r, _, _ = esq.split()
            _, g, b = dir_.split()
            img = Image.merge("RGB", (r, g, b))
        saida = io.BytesIO()
        img.save(saida, "JPEG", quality=85)
        return saida.getvalue()

    def _publicar(self, jpeg):
        with self.novo_quadro:
            self.quadro = jpeg
            self.numero += 1
            self.novo_quadro.notify_all()

    def _aceitar(self):
        while True:
            try:
                conn, origem = self.servidor_tcp.accept()
            except OSError:
                time.sleep(1)
                continue
            print(f"Camera: 3DS conectado ({origem[0]})")
            self.status["conectado"] = True
            conn.settimeout(5)
            quadros, t0 = 0, time.monotonic()
            try:
                while True:
                    magic, olho, total, _, tam = struct.unpack(CABECALHO, receber_tudo(conn, TAM_CABECALHO))
                    if magic != b"3DSF" or tam > 2_000_000 or olho > 1:
                        raise ConnectionError("pacote invalido")
                    self.olhos[olho] = receber_tudo(conn, tam)
                    if total < 2:
                        self.olhos[1] = None
                    if olho == total - 1:             # chegou o último olho do quadro
                        self._publicar(self._compor(total))
                        quadros += 1
                        if time.monotonic() - t0 >= 2:
                            self.status["fps"] = quadros / (time.monotonic() - t0)
                            quadros, t0 = 0, time.monotonic()
            except Exception as e:      # qualquer erro encerra só esta conexão
                print("Camera: desconectou", repr(e))
            finally:
                conn.close()
                self.status.update(conectado=False, fps=0.0)
