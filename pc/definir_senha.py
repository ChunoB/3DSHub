"""Define a senha fixa do acesso pela internet deste PC (janela).

Com a senha, qualquer 3DS pareia de qualquer lugar: no 3DS, Ajustes > Acesso pela
internet > digite a senha. O PC não guarda a senha, só o canal que vem dela.
Abre pelo menu do ícone do Hub ("Senha do acesso pela internet...") ou com:
    py definir_senha.py      (com o 3DS Hub aberto)
"""
import socket
import tkinter as tk
from tkinter import messagebox

from remoto import SENHA_MINIMA

TITULO = "3DS Hub - senha do acesso pela internet"


def enviar(senha):
    """Manda a senha para o Hub aberto. True se ele definiu e publicou."""
    with socket.create_connection(("127.0.0.1", 5010), timeout=40) as s:
        s.sendall(b"S " + senha.encode("utf-8").hex().encode() + b"\n")
        resp = b""
        while len(resp) < 8:
            parte = s.recv(8 - len(resp))
            if not parte:
                break
            resp += parte
    return resp[:4] == b"3DSS" and resp[4:8] == b"\x01\x00\x00\x00"


def main():
    raiz = tk.Tk()
    raiz.title(TITULO)
    raiz.resizable(False, False)
    raiz.attributes("-topmost", True)
    quadro = tk.Frame(raiz, padx=16, pady=12)
    quadro.pack()
    tk.Label(quadro, justify="left", text=(
        "Escolha uma senha para este PC. No 3DS, longe ou perto de casa:\n"
        "Ajustes > Acesso pela internet > digite esta senha.\n\n"
        f"Use pelo menos {SENHA_MINIMA} caracteres. Quem souber a senha controla\n"
        "este PC pelo 3DS Hub, então não use uma senha fácil.")).grid(row=0, column=0, columnspan=2, sticky="w")
    tk.Label(quadro, text="Senha:").grid(row=1, column=0, sticky="e", pady=(10, 2))
    tk.Label(quadro, text="Repita:").grid(row=2, column=0, sticky="e")
    s1 = tk.Entry(quadro, show="*", width=30)
    s2 = tk.Entry(quadro, show="*", width=30)
    s1.grid(row=1, column=1, pady=(10, 2))
    s2.grid(row=2, column=1)
    s1.focus_set()

    def salvar(_evento=None):
        a, b = s1.get(), s2.get()
        if a != b:
            messagebox.showwarning(TITULO, "As duas senhas não são iguais.", parent=raiz)
            return
        if len(a.strip()) < SENHA_MINIMA:
            messagebox.showwarning(TITULO, f"Use pelo menos {SENHA_MINIMA} caracteres.", parent=raiz)
            return
        try:
            ok = enviar(a)
        except OSError:
            messagebox.showerror(TITULO, "O 3DS Hub do PC não está aberto.", parent=raiz)
            return
        if ok:
            messagebox.showinfo(TITULO, "Senha definida! O acesso pela internet está ligado.\n\n"
                                        "No 3DS: Ajustes > Acesso pela internet > digite a senha.", parent=raiz)
            raiz.destroy()
        else:
            messagebox.showerror(TITULO, "Não consegui publicar a senha (o PC está sem internet?).\n"
                                         "Veja o log do Hub.", parent=raiz)

    botoes = tk.Frame(quadro)
    botoes.grid(row=3, column=0, columnspan=2, pady=(12, 0), sticky="e")
    tk.Button(botoes, text="Cancelar", width=10, command=raiz.destroy).pack(side="right", padx=(6, 0))
    tk.Button(botoes, text="Salvar", width=10, command=salvar).pack(side="right")
    raiz.bind("<Return>", salvar)
    raiz.mainloop()


if __name__ == "__main__":
    main()
