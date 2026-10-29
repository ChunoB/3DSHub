#!/usr/bin/env python3
"""
3DS Hub — servidor único do PC, fica no ícone da bandeja (perto do relógio).

Junta tudo: Descoberta (5009), Ajustes e atualização do app (5010),
Controle (5005), Segunda Tela (5006), Câmera (5007 + OBS em
http://127.0.0.1:8090/obs), Espelho (5008), Steam (5012) e Som (5013).

Rodar escondido (sem janela):  pythonw hub.py
Rodar vendo os logs:            py hub.py
"""

import ctypes
import os
import subprocess
import sys
import threading
import time

PASTA = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, PASTA)
LOG = os.path.join(PASTA, "hub.log")

# Sem janela (pythonw) não existe terminal: os prints vão para o hub.log
if sys.stdout is None or not sys.stdout.isatty():
    try:   # o log não cresce para sempre: passando de 1 MB, guarda só o final
        if os.path.getsize(LOG) > 1_000_000:
            with open(LOG, "rb") as f:
                f.seek(-200_000, os.SEEK_END)
                final = f.read()
            with open(LOG, "wb") as f:
                f.write(final)
    except OSError:
        pass
    sys.stdout = sys.stderr = open(LOG, "a", encoding="utf-8", buffering=1, errors="replace")
print(f"\n===== 3DS Hub iniciando {time.strftime('%d/%m/%Y %H:%M:%S')} =====")

import pystray                                   # noqa: E402
from PIL import Image, ImageDraw                 # noqa: E402

from camera import Camera                        # noqa: E402
from config import ARQUIVO as ARQ_CONFIG, Config  # noqa: E402
from espelho import Espelho                      # noqa: E402
from descoberta import Descoberta                # noqa: E402
from ajustes import Ajustes                      # noqa: E402
from remoto import Remoto                        # noqa: E402
from steam import Steam                          # noqa: E402
from som import Som                              # noqa: E402

try:
    from controle import Controle                # noqa: E402
except Exception as e:
    # o vgamepad já falha ao ser importado se o driver ViGEmBus não existe;
    # sem isso o Hub inteiro cairia, não só o Controle
    _erro_controle = repr(e)

    class Controle:
        def __init__(self, _config):
            raise RuntimeError(f"driver ViGEmBus faltando? {_erro_controle}")
from tela import SegundaTela                     # noqa: E402
from util import ip_para_o_3ds, ips_locais       # noqa: E402

ATALHO_INICIAR = os.path.join(os.environ["APPDATA"], r"Microsoft\Windows\Start Menu\Programs\Startup",
                              "3DS Hub.lnk")


# ---------------------------------------------------------------- iniciar com o Windows
def pythonw():
    exe = sys.executable
    candidato = os.path.join(os.path.dirname(exe), "pythonw.exe")
    return candidato if os.path.exists(candidato) else exe


def iniciar_com_windows(ligar):
    if ligar:
        import win32com.client
        atalho = win32com.client.Dispatch("WScript.Shell").CreateShortCut(ATALHO_INICIAR)
        atalho.TargetPath = pythonw()
        atalho.Arguments = f'"{os.path.join(PASTA, "hub.py")}"'
        atalho.WorkingDirectory = PASTA
        atalho.Description = "3DS Hub"
        atalho.save()
    elif os.path.exists(ATALHO_INICIAR):
        os.remove(ATALHO_INICIAR)


# ---------------------------------------------------------------- ícone
def desenhar_icone():
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((6, 2, 58, 30), 6, fill=(200, 30, 40))       # metade de cima
    d.rectangle((12, 7, 52, 25), fill=(30, 30, 40))                  # tela de cima
    d.rounded_rectangle((6, 34, 58, 62), 6, fill=(200, 30, 40))      # metade de baixo
    d.rectangle((18, 38, 46, 56), fill=(60, 190, 230))               # tela de toque
    d.ellipse((8, 40, 15, 47), fill=(240, 240, 240))                 # circle pad
    return img


def main():
    config = Config()
    if config["iniciar_com_windows"] != os.path.exists(ATALHO_INICIAR):
        try:
            iniciar_com_windows(config["iniciar_com_windows"])
        except Exception as e:
            print("Nao consegui configurar o inicio com o Windows:", e)

    class Falhou:
        """Fica no lugar de um serviço que não iniciou, para o menu continuar funcionando."""
        status = {}
        def soltar_tudo(self): pass
        def url_obs(self): return f"http://127.0.0.1:{config['camera']['porta_http']}/obs"

    classes = {"Controle": Controle, "Segunda Tela": SegundaTela, "Camera": Camera, "Espelho": Espelho,
               "Descoberta": Descoberta, "Ajustes": Ajustes, "Remoto": Remoto, "Steam": Steam,
               "Som": Som}
    servicos, erros = {}, []
    for nome, classe in classes.items():
        servicos[nome] = Falhou()
        try:
            servicos[nome] = classe(config)
            servicos[nome].iniciar()
        except Exception as e:
            # quase sempre: outro servidor (PadRemoto, SegundaTela ou outro Hub) já usa a porta;
            # no Controle também pode ser o driver ViGEmBus faltando
            erros.append(nome)
            print(f"{nome}: nao iniciou ({e!r})")
    redes = ", ".join(f"{nome} {ip}" for nome, ip in ips_locais())
    print(f"Ouvindo em: {redes}. Servicos com erro: {erros or 'nenhum'}")

    if len(erros) == len(classes):
        # nada subiu: quase certeza de que o Hub já está aberto (ícone perto do relógio)
        print("Nenhum servico iniciou; o 3DS Hub provavelmente ja esta rodando. Saindo.")
        ctypes.windll.user32.MessageBoxW(
            None, "O 3DS Hub já está rodando (ícone perto do relógio).", "3DS Hub", 0x40)
        return

    # pelo nome (não pela ordem), para não quebrar quando entrar serviço novo
    controle, tela = servicos["Controle"], servicos["Segunda Tela"]
    camera, espelho = servicos["Camera"], servicos["Espelho"]
    remoto, steam, som = servicos["Remoto"], servicos["Steam"], servicos["Som"]

    def linha_remoto(_):
        if "Remoto" in erros:
            return "Pela internet: porta 5011 ocupada"
        s = remoto.status
        if not remoto.ligado:
            return "Pela internet: desligado (pareie pelo 3DS em Ajustes)"
        return f"Pela internet: {s.get('estado', '')}" + (" - 3DS conectado" if s.get("conectado") else "")

    def alternar_remoto(_icone, _item):
        remoto.alternar()

    def trocar_codigo(icone, _item):
        remoto.trocar_codigo()
        icone.notify("Código novo criado. Pareie o 3DS de novo em casa (Ajustes > Acesso pela internet).",
                     "3DS Hub")

    def codigo_longe(_icone, _item):
        # numa thread: publicar no ntfy pode levar alguns segundos e a caixa espera o OK
        def mostrar():
            codigo = remoto.codigo_pareamento()
            if codigo:
                texto = (f"Código: {codigo}\n\nNo 3DS: Ajustes > Acesso pela internet > Digitar código.\n"
                         "Vale por 30 minutos.")
            else:
                texto = "Não consegui criar o código (o PC está sem internet?). Veja o log."
            ctypes.windll.user32.MessageBoxW(None, texto, "3DS Hub - parear longe de casa", 0x40)
        threading.Thread(target=mostrar, daemon=True).start()

    def linha_status(nome, s, extra=""):
        if nome in erros:
            return f"{nome}: porta ocupada"
        return f"{nome}: {'conectado' if s.status.get('conectado') else 'esperando'}{extra}"

    def ao_sair(icone, _):
        icone.visible = False
        icone.stop()
        try:
            controle.soltar_tudo()
        except Exception:
            pass
        os._exit(0)

    def alternar_inicio(_icone, _item):
        dados = {k: config[k] for k in config._dados}
        dados["iniciar_com_windows"] = not config["iniciar_com_windows"]
        config.salvar(dados)
        iniciar_com_windows(dados["iniciar_com_windows"])

    def copiar_link_obs(_icone, _item):
        subprocess.run("clip", input=camera.url_obs(), text=True, shell=True)

    menu = pystray.Menu(
        # o IP que o 3DS deve usar (o do hotspot, se ligado) e, embaixo, as outras redes
        pystray.MenuItem(lambda _: f"3DS Hub  -  IP para o 3DS: {ip_para_o_3ds()}", None, enabled=False),
        pystray.MenuItem(lambda _: "Outras redes: " + ", ".join(
                             f"{nome} {ip}" for nome, ip in ips_locais() if ip != ip_para_o_3ds()),
                         None, enabled=False,
                         visible=lambda _: len(ips_locais()) > 1),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem(lambda _: linha_status(
            "Controle", controle,
            f" - {controle.status['perfil']}" if controle.status.get("conectado") else ""), None, enabled=False),
        pystray.MenuItem(lambda _: linha_status("Segunda Tela", tela), None, enabled=False),
        pystray.MenuItem(lambda _: linha_status(
            "Camera", camera,
            f" ({camera.status['fps']:.1f} fps)" if camera.status.get("conectado") else ""), None, enabled=False),
        pystray.MenuItem(lambda _: linha_status("Espelho", espelho), None, enabled=False),
        pystray.MenuItem(lambda _: linha_status(
            "Steam", steam,
            f" - {steam.status['jogo']}" if steam.status.get("jogo") else ""), None, enabled=False),
        pystray.MenuItem(lambda _: linha_status("Som", som), None, enabled=False),
        pystray.MenuItem(linha_remoto, None, enabled=False),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("Acesso pela internet", alternar_remoto,
                         checked=lambda _: getattr(remoto, "ligado", False),
                         enabled=lambda _: "Remoto" not in erros),
        pystray.MenuItem(lambda _: "Senha do acesso pela internet..." if not getattr(remoto, "tem_senha", False)
                         else "Trocar a senha do acesso pela internet...",
                         lambda: subprocess.Popen([pythonw(), os.path.join(PASTA, "definir_senha.py")],
                                                  cwd=PASTA),
                         enabled=lambda _: "Remoto" not in erros),
        pystray.MenuItem("Parear um 3DS longe de casa (codigo)...", codigo_longe,
                         enabled=lambda _: "Remoto" not in erros),
        pystray.MenuItem("Trocar o codigo do acesso pela internet", trocar_codigo,
                         visible=lambda _: getattr(remoto, "ligado", False)),
        pystray.MenuItem("Copiar link da camera pro OBS", copiar_link_obs),
        pystray.MenuItem("Testar controle (joy.cpl)", lambda: subprocess.Popen(
            ["rundll32.exe", "shell32.dll,Control_RunDLL", "joy.cpl"])),
        pystray.MenuItem("Editar configuracoes", lambda: os.startfile(ARQ_CONFIG)),
        pystray.MenuItem("Ver log", lambda: os.startfile(LOG)),
        pystray.MenuItem("Iniciar com o Windows", alternar_inicio,
                         checked=lambda _: config["iniciar_com_windows"]),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("Sair", ao_sair),
    )

    icone = pystray.Icon("3dshub", desenhar_icone(), "3DS Hub", menu)

    def atualizar_menu():
        while True:
            time.sleep(2)
            try:
                icone.update_menu()
            except Exception:
                pass

    def aviso_inicial():
        time.sleep(2)
        if erros:
            icone.notify(f"Nao iniciou: {', '.join(erros)}. Veja o log (icone > Ver log). "
                         "Causas comuns: PadRemoto/SegundaTela antigos abertos ou driver ViGEmBus faltando.",
                         "3DS Hub")

    threading.Thread(target=atualizar_menu, daemon=True).start()
    threading.Thread(target=aviso_inicial, daemon=True).start()
    icone.run()


if __name__ == "__main__":
    main()
