@echo off
chcp 65001 >nul
title Instalador do 3DS Hub
echo === Instalando o 3DS Hub ===
echo.

where py >nul 2>nul
if errorlevel 1 (
    echo Python nao encontrado.
    echo Instale o Python 3 em https://www.python.org/downloads/
    echo e marque "Add Python to PATH" durante a instalacao.
    echo Depois rode este instalador de novo.
    echo.
    pause
    exit /b 1
)

echo Instalando as bibliotecas (pode demorar uns minutos)...
py -3 -m pip install --upgrade -r "%~dp0requirements.txt"
if errorlevel 1 (
    echo.
    echo Deu erro ao instalar as bibliotecas. Veja a mensagem acima.
    pause
    exit /b 1
)

echo.
echo Conferindo o driver do controle virtual (ViGEmBus)...
powershell -NoProfile -Command "if (Get-PnpDevice -FriendlyName '*Virtual Gamepad Emulation*' -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }"
if errorlevel 1 (
    echo.
    echo O driver ViGEmBus nao esta instalado. Sem ele o Controle nao funciona
    echo ^(o resto do Hub funciona normalmente^).
    echo Baixe e instale o "ViGEmBus_Setup" em:
    echo   https://github.com/nefarius/ViGEmBus/releases
    echo Depois reinicie o 3DS Hub ^(icone ^> Sair, e abra de novo^).
    echo.
    start "" https://github.com/nefarius/ViGEmBus/releases
) else (
    echo Driver OK.
)

echo.
echo Ligando o vigia (reabre o Hub sozinho se ele fechar)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0vigia_instalar.ps1"
if errorlevel 1 echo Nao deu para ligar o vigia. O Hub funciona normalmente sem ele.

echo.
echo Pronto! Abrindo o 3DS Hub (icone perto do relogio)...
echo Ele vai ligar sozinho junto com o Windows daqui pra frente.
start "" pyw -3 "%~dp0hub.py"
echo.
echo Se o Windows perguntar sobre o firewall, permita na rede privada.
echo No 3DS, abra o 3DS Hub, aperte Y e digite o IP deste PC
echo (ele aparece no menu do icone do Hub).
echo.
pause
