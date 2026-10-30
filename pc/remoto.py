"""Acesso pela internet: o 3DS fala com o Hub de qualquer lugar, sem mexer no roteador.

Como funciona:
  * Um "portão" (WebSocket) escuta só no próprio PC, em 127.0.0.1:5011.
  * O cloudflared (da Cloudflare, grátis e sem conta) cria um endereço https público
    que leva até esse portão. A conexão do 3DS até a Cloudflare é criptografada.
  * O endereço muda quando o cloudflared reinicia. Por isso o PC publica o endereço
    atual num canal secreto do ntfy.sh, e o 3DS lê de lá.
  * Para entrar é preciso o código (token) que o 3DS recebe ao parear em casa.
    Sem ele o portão recusa tudo.
  * Cada conexão do 3DS pede uma porta (5005 a 5010, 5012 da Steam e 5013 do Som). O portão liga essa conexão
    ao serviço de sempre do Hub no próprio PC, então nada mais precisa mudar.
    Portas TCP viram um fluxo de bytes, portas UDP viram uma mensagem por pacote.

Pedido do 3DS (sobre TLS):
    GET /t?porta=5006 HTTP/1.1
    Authorization: Bearer <token>
    Upgrade: websocket ...
"""

import asyncio
import base64
import hashlib
import hmac
import json
import os
import re
import secrets
import socket
import subprocess
import threading
import time
import urllib.request

from config import PASTA
from util import SSL

ARQUIVO = os.path.join(PASTA, "remoto.json")
CLOUDFLARED = os.path.join(PASTA, "cloudflared.exe")
URL_CLOUDFLARED = "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe"
NTFY = "https://ntfy.sh/"
PORTA_PORTAO = 5011
PORTAS_TCP = {5007, 5008, 5010, 5012}  # Câmera, Espelho, Ajustes, Steam
PORTAS_UDP = {5005, 5006, 5013, 5014, 5015, 5016}  # Controle, Segunda Tela, Som, Bichinho, Pescaria, Karaokê
ESPERA_TUNEL = (10, 180)             # reabrir o túnel: 10 s, dobrando a cada falha seguida, até 3 min
REPUBLICAR_S = 3 * 3600               # o ntfy guarda as mensagens por 12 h
GUID_WS = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"   # fixo, do padrão WebSocket (RFC 6455)
ALFABETO_CODIGO = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"  # sem 0/O e 1/I, que confundem
                                                     # 8 letras = 40 bits: impossível de chutar em 30 min

_instancia = None


def topico_do_codigo(codigo):
    """Canal do ntfy de um código de pareamento (o 3DS calcula o mesmo)."""
    return "3dshub-par-" + codigo.replace("-", "").lower()


# Senha fixa do PC: o canal do ntfy vem da senha por PBKDF2 (lento de propósito, para chutes
# custarem caro). Nem o PC guarda a senha: só o canal. O 3DS calcula o mesmo canal.
SAL_SENHA = b"3dshub-senha-v1"
ITERACOES_SENHA = 20000
SENHA_MINIMA = 6


def topico_da_senha(senha):
    chave = hashlib.pbkdf2_hmac("sha256", senha.strip().encode("utf-8"), SAL_SENHA, ITERACOES_SENHA)
    return "3dshub-pw-" + chave.hex()[:32]


def obter():
    """O serviço de acesso remoto que o hub.py criou (ou None)."""
    return _instancia


# ---------------------------------------------------------------- WebSocket (servidor mínimo)
async def ler_quadro(leitor):
    """Lê um quadro do 3DS. Volta (opcode, dados)."""
    cab = await leitor.readexactly(2)
    opcode = cab[0] & 0x0F
    mascarado = cab[1] & 0x80
    n = cab[1] & 0x7F
    if n == 126:
        n = int.from_bytes(await leitor.readexactly(2), "big")
    elif n == 127:
        n = int.from_bytes(await leitor.readexactly(8), "big")
    if n > 4 * 1024 * 1024:
        raise ValueError("quadro grande demais")
    chave = await leitor.readexactly(4) if mascarado else None
    dados = await leitor.readexactly(n)
    if chave:
        dados = bytes(b ^ chave[i % 4] for i, b in enumerate(dados))
    return opcode, dados


def quadro(dados, opcode=2):
    n = len(dados)
    if n < 126:
        cab = bytes([0x80 | opcode, n])
    elif n < 65536:
        cab = bytes([0x80 | opcode, 126]) + n.to_bytes(2, "big")
    else:
        cab = bytes([0x80 | opcode, 127]) + n.to_bytes(8, "big")
    return cab + dados


class _UDP(asyncio.DatagramProtocol):
    def __init__(self, ao_receber):
        self.ao_receber = ao_receber

    def datagram_received(self, dados, origem):
        self.ao_receber(dados)


class Remoto:
    def __init__(self, config):
        global _instancia
        self.config = config
        self.status = {"conectado": False, "estado": "desligado", "url": ""}
        self.dados = self._carregar()
        self.processo = None
        self.loop = None
        self.conexoes = 0
        self._parar = threading.Event()
        self._tunel_rodando = False
        self._publicado = 0.0
        self._senha_publicada = 0.0
        _instancia = self

    # ------------------------------------------------ configuração (remoto.json)
    def _carregar(self):
        try:
            with open(ARQUIVO, encoding="utf-8") as f:
                d = json.load(f)
        except (OSError, ValueError):
            d = {}
        d.setdefault("ligado", False)
        d.setdefault("token", "")
        d.setdefault("topico", "")
        d.setdefault("topico_senha", "")      # canal da senha fixa ("" = sem senha)
        return d

    def _salvar(self):
        with open(ARQUIVO, "w", encoding="utf-8") as f:
            json.dump(self.dados, f, indent=2)

    @property
    def ligado(self):
        return bool(self.dados["ligado"])

    def parear(self):
        """Liga o acesso remoto (criando o código na primeira vez) e devolve o que o 3DS guarda."""
        if not self.dados["token"]:
            self.dados["token"] = secrets.token_hex(24)
            self.dados["topico"] = "3dshub-" + secrets.token_hex(12)
        self.dados["ligado"] = True
        self._salvar()
        self._ligar()
        return self.dados["token"], self.dados["topico"]

    def alternar(self):
        if self.ligado:
            self.dados["ligado"] = False
            self._salvar()
            self._desligar()
        else:
            self.parear()

    def trocar_codigo(self):
        """Gera um código novo: os 3DS pareados antes deixam de entrar."""
        self.dados["token"] = ""
        self.parear()
        if self.dados["topico_senha"]:
            self._publicar_senha()     # a senha continua valendo, já com o código novo

    @property
    def tem_senha(self):
        return bool(self.dados["topico_senha"])

    def definir_senha(self, senha):
        """Liga o acesso e guarda o canal da senha. Volta True se publicou no ntfy."""
        if len(senha.strip()) < SENHA_MINIMA:
            return False
        self.parear()
        self.dados["topico_senha"] = topico_da_senha(senha)
        self._salvar()
        print("Remoto: senha do acesso pela internet definida")
        return self._publicar_senha()

    def apagar_senha(self):
        """Tira a senha e troca o código: o pareamento antigo que ficou no ntfy (até 12 h)
        deixa de servir para entrar."""
        self.dados["topico_senha"] = ""
        self.trocar_codigo()
        print("Remoto: senha apagada e codigo trocado")

    def _publicar_senha(self):
        """Põe os dados do pareamento no canal da senha (o ntfy guarda 12 h: republica sempre)."""
        nome = socket.gethostname()[:30].replace(" ", "")
        try:
            req = urllib.request.Request(NTFY + self.dados["topico_senha"],
                                         data=f"3DSPAR1 {self.dados['token']} {self.dados['topico']} {nome}".encode(),
                                         headers={"User-Agent": "3DSHub", "Cache": "yes"}, method="POST")
            urllib.request.urlopen(req, timeout=20, context=SSL).read()
            self._senha_publicada = time.monotonic()
            return True
        except Exception as e:
            print("Remoto: nao consegui publicar o pareamento da senha:", e)
            self._senha_publicada = 0.0
            return False

    def codigo_pareamento(self):
        """Pareamento longe de casa: liga o acesso, cria um código curto (ex.: K7QM-2XPA) e põe
        os dados do pareamento no canal do ntfy desse código. O 3DS digita o código e lê de lá
        (só aceita mensagens dos últimos 30 minutos). Volta o código, ou None se o ntfy falhou."""
        token, topico = self.parear()
        codigo = "".join(secrets.choice(ALFABETO_CODIGO) for _ in range(8))
        nome = socket.gethostname()[:30].replace(" ", "")
        try:
            req = urllib.request.Request(NTFY + topico_do_codigo(codigo),
                                         data=f"3DSPAR1 {token} {topico} {nome}".encode(),
                                         headers={"User-Agent": "3DSHub", "Cache": "yes"}, method="POST")
            urllib.request.urlopen(req, timeout=20, context=SSL).read()
        except Exception as e:
            print("Remoto: nao consegui publicar o codigo de pareamento:", e)
            return None
        print("Remoto: codigo de pareamento criado (vale 30 minutos)")
        return codigo[:4] + "-" + codigo[4:]

    # ------------------------------------------------ serviço
    def iniciar(self):
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self._rodar_loop, daemon=True, name="remoto").start()
        threading.Thread(target=self._republicar, daemon=True, name="remoto-publicar").start()
        if self.ligado:
            self._ligar()

    def _republicar(self):
        """O ntfy esquece as mensagens depois de 12 h: publica o endereço de novo de tempos em tempos
        (e tenta de novo logo se a última vez falhou)."""
        while True:
            time.sleep(60)
            if self.ligado and self.dados["topico_senha"]:
                espera = REPUBLICAR_S if self._senha_publicada else 120
                if time.monotonic() - self._senha_publicada > espera:
                    self._publicar_senha()
            url = self.status["url"]
            if not url or not self.ligado:
                continue
            espera = REPUBLICAR_S if self._publicado else 120
            if time.monotonic() - self._publicado > espera:
                self._publicado = time.monotonic() if self._publicar(url) else 0.0

    def _rodar_loop(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_until_complete(asyncio.start_server(self._atender, "127.0.0.1", PORTA_PORTAO))
        self.loop.run_forever()

    def _ligar(self):
        self._parar.clear()
        if not self._tunel_rodando:
            self._tunel_rodando = True
            threading.Thread(target=self._manter_tunel, daemon=True, name="tunel").start()

    def _desligar(self):
        self._parar.set()
        self.status.update(estado="desligado", url="")
        if self.processo:
            try:
                self.processo.terminate()
            except OSError:
                pass

    # ------------------------------------------------ cloudflared
    def _baixar_cloudflared(self):
        if os.path.exists(CLOUDFLARED):
            return True
        self.status["estado"] = "baixando o cloudflared..."
        print("Remoto: baixando o cloudflared da Cloudflare (so na primeira vez)...")
        try:
            temp = CLOUDFLARED + ".baixando"
            req = urllib.request.Request(URL_CLOUDFLARED, headers={"User-Agent": "3DSHub"})
            with urllib.request.urlopen(req, timeout=120, context=SSL) as r, open(temp, "wb") as f:
                while True:
                    bloco = r.read(1 << 16)
                    if not bloco:
                        break
                    f.write(bloco)
            os.replace(temp, CLOUDFLARED)
            return True
        except Exception as e:
            print("Remoto: nao consegui baixar o cloudflared:", e)
            self.status["estado"] = "sem cloudflared (veja o log)"
            return False

    def _publicar(self, url):
        """Põe o endereço atual no canal secreto do ntfy, de onde o 3DS lê."""
        try:
            req = urllib.request.Request(NTFY + self.dados["topico"], data=url.encode(),
                                         headers={"User-Agent": "3DSHub", "Cache": "yes"}, method="POST")
            urllib.request.urlopen(req, timeout=20, context=SSL).read()
            return True
        except Exception as e:
            print("Remoto: nao consegui publicar o endereco:", e)
            return False

    def _manter_tunel(self):
        espera = ESPERA_TUNEL[0]
        while not self._parar.is_set():
            if not self._baixar_cloudflared():
                self._parar.wait(60)
                continue
            self.status["estado"] = "abrindo o tunel..."
            cmd = [CLOUDFLARED, "tunnel", "--no-autoupdate", "--protocol", "http2",
                   "--url", f"http://127.0.0.1:{PORTA_PORTAO}"]
            try:
                self.processo = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                                 text=True, errors="replace",
                                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            except OSError as e:
                print("Remoto: nao consegui abrir o cloudflared:", e)
                self.status["estado"] = "erro ao abrir o tunel"
                self._parar.wait(60)
                continue
            for linha in self.processo.stderr:
                if os.environ.get("HUB_DEBUG"):
                    print("cloudflared:", linha.rstrip())
                m = re.search(r"https://(?!api\.)[a-z0-9-]+\.trycloudflare\.com", linha)
                if m and m.group(0) != self.status["url"]:
                    self.status["url"] = m.group(0)
                    self.status["estado"] = "ligado"
                    print("Remoto: endereco", self.status["url"])
                    espera = ESPERA_TUNEL[0]            # abriu: se cair, volta logo
                    self._publicado = time.monotonic() if self._publicar(self.status["url"]) else 0.0
                if self._parar.is_set():
                    break
            self.processo.wait()
            abriu, self.status["url"] = bool(self.status["url"]), ""
            if not self._parar.is_set():
                # falha seguida (a rede pode estar bloqueando a Cloudflare): espera cada vez mais
                if abriu:
                    print(f"Remoto: o tunel caiu, abrindo de novo em {espera} s")
                else:
                    print(f"Remoto: o tunel nao abriu (a rede pode estar bloqueando), tentando de novo em {espera} s")
                self.status["estado"] = "reconectando..."
                self._parar.wait(espera)
                espera = min(espera * 2, ESPERA_TUNEL[1])
        self._tunel_rodando = False

    # ------------------------------------------------ portão (uma conexão por pedido do 3DS)
    async def _atender(self, leitor, escritor):
        try:
            cab = await asyncio.wait_for(leitor.readuntil(b"\r\n\r\n"), 15)
        except Exception:
            escritor.close()
            return
        linhas = cab.decode("latin-1").split("\r\n")
        if os.environ.get("HUB_DEBUG"):
            print("portao: pedido:", " | ".join(l for l in linhas if not l.lower().startswith("authorization")))
        cabecalhos = {}
        for l in linhas[1:]:
            if ":" in l:
                k, v = l.split(":", 1)
                cabecalhos[k.strip().lower()] = v.strip()
        m = re.match(r"GET /t\?porta=(\d+)", linhas[0])
        token = self.dados["token"]
        enviado = cabecalhos.get("authorization", "")
        if (not self.ligado or not token or not m or
                not hmac.compare_digest(enviado.encode(), f"Bearer {token}".encode())):
            escritor.write(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            await escritor.drain()
            escritor.close()
            return
        porta = int(m.group(1))
        if porta not in PORTAS_TCP | PORTAS_UDP or "sec-websocket-key" not in cabecalhos:
            escritor.write(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            await escritor.drain()
            escritor.close()
            return
        aceite = base64.b64encode(hashlib.sha1(cabecalhos["sec-websocket-key"].encode() + GUID_WS).digest())
        escritor.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       b"Sec-WebSocket-Accept: " + aceite + b"\r\n\r\n")
        await escritor.drain()

        self.conexoes += 1
        self.status["conectado"] = True
        print(f"Remoto: 3DS conectado pela internet (porta {porta})")
        try:
            if porta in PORTAS_TCP:
                await self._ligar_tcp(leitor, escritor, porta)
            else:
                await self._ligar_udp(leitor, escritor, porta)
        except Exception as e:
            print(f"Remoto: conexao da porta {porta} terminou ({e!r})")
        finally:
            self.conexoes -= 1
            self.status["conectado"] = self.conexoes > 0
            escritor.close()

    async def _ler_mensagens(self, leitor, escritor, ao_receber):
        while True:
            opcode, dados = await ler_quadro(leitor)
            if opcode == 8:                       # fechar
                return
            if opcode == 9:                       # ping -> pong
                escritor.write(quadro(dados, 10))
                await escritor.drain()
            elif opcode in (0, 1, 2):
                await ao_receber(dados)

    async def _ligar_tcp(self, leitor, escritor, porta):
        sl, se = await asyncio.open_connection("127.0.0.1", porta)

        async def do_3ds(dados):
            se.write(dados)
            await se.drain()

        async def do_servico():
            while True:
                dados = await sl.read(64 * 1024)
                if not dados:
                    return
                escritor.write(quadro(dados))
                await escritor.drain()

        tarefas = [asyncio.ensure_future(self._ler_mensagens(leitor, escritor, do_3ds)),
                   asyncio.ensure_future(do_servico())]
        try:
            await asyncio.wait(tarefas, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for t in tarefas:
                t.cancel()
            se.close()

    async def _ligar_udp(self, leitor, escritor, porta):
        def resposta(dados):
            escritor.write(quadro(dados))

        transporte, _ = await self.loop.create_datagram_endpoint(
            lambda: _UDP(resposta), remote_addr=("127.0.0.1", porta))

        async def do_3ds(dados):
            transporte.sendto(dados)
            await escritor.drain()

        try:
            await self._ler_mensagens(leitor, escritor, do_3ds)
        finally:
            transporte.close()
