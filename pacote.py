"""Monta dist/3dshub-pacote.zip: 3dshub/3dshub.3dsx, 3dshub.cia, LEIA-ME.txt e a pasta pc.
Uso (na pasta do projeto, depois do make e do criar_cia.bat): py pacote.py
"""
import os
import zipfile

raiz = os.path.dirname(os.path.abspath(__file__))
os.makedirs(os.path.join(raiz, "dist"), exist_ok=True)
saida = os.path.join(raiz, "dist", "3dshub-pacote.zip")
with zipfile.ZipFile(saida, "w", zipfile.ZIP_DEFLATED) as z:
    for nome in ("3dshub.3dsx", "3dshub.cia"):
        z.write(os.path.join(raiz, "3ds", nome), f"3dshub/{nome}")
    z.write(os.path.join(raiz, "LEIA-ME.txt"), "3dshub/LEIA-ME.txt")
    for pasta, subs, arquivos in os.walk(os.path.join(raiz, "pc")):
        subs[:] = [s for s in subs if s != "__pycache__"]
        for a in arquivos:
            cheio = os.path.join(pasta, a)
            z.write(cheio, "3dshub/" + os.path.relpath(cheio, raiz).replace("\\", "/"))
print(saida)
