"""Mostra um código para parear o 3DS pela internet (quando o 3DS não está na rede de casa).

Uso: py parear_codigo.py   (com o 3DS Hub aberto)
No 3DS: Ajustes > Acesso pela internet > Digitar código. O código vale por 30 minutos.
O mesmo código aparece no menu do ícone do Hub: "Parear um 3DS longe de casa...".
"""
import socket
import struct
import sys


def pedir_codigo():
    with socket.create_connection(("127.0.0.1", 5010), timeout=40) as s:
        s.sendall(b"P\n")
        cab = b""
        while len(cab) < 8:
            parte = s.recv(8 - len(cab))
            if not parte:
                return None
            cab += parte
        if cab[:4] != b"3DSP":
            return None
        n = struct.unpack("<I", cab[4:])[0]
        texto = b""
        while len(texto) < n:
            parte = s.recv(n - len(texto))
            if not parte:
                break
            texto += parte
        return texto.decode() or None


if __name__ == "__main__":
    try:
        codigo = pedir_codigo()
    except OSError:
        print("O 3DS Hub do PC nao esta aberto (ou e uma versao antiga).")
        sys.exit(1)
    if not codigo:
        print("Nao consegui criar o codigo (sem internet?). Veja o hub.log.")
        sys.exit(1)
    print(f"Codigo: {codigo}")
    print("No 3DS: Ajustes > Acesso pela internet > Digitar codigo. Vale por 30 minutos.")
