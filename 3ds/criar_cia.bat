@echo off
rem Gera o 3dshub.cia (para instalar no HOME Menu pelo FBI).
rem Antes rode "make" (gera o 3dshub.elf). Usa o makerom e o bannertool da pasta ..\ferramentas.
setlocal
cd /d "%~dp0"
set FERR=..\ferramentas
set BT=%FERR%\bannertool\windows-x86_64\bannertool.exe
set MR=%FERR%\makerom\makerom.exe

if not exist 3dshub.elf (echo Falta o 3dshub.elf: rode make antes. & exit /b 1)
if not exist arte\banner.png (echo Falta a arte: rode "py arte\gerar_arte.py". & exit /b 1)

"%BT%" makebanner -i arte\banner.png -a arte\banner.wav -o build\banner.bnr || exit /b 1
rem textos do HOME Menu sem acento (o bannertool le a linha de comando sem UTF-8 no Windows)
"%BT%" makesmdh -s "3DS Hub" -l "Controle, Segunda Tela, Sintetizador, Camera, Espelho e Steam" -p "Bruno" -i arte\icon.png -o build\icon.icn || exit /b 1
rem versao do CIA = VERSION_MAJOR/MINOR/MICRO do Makefile
for /f "tokens=3" %%v in ('findstr /b "VERSION_MAJOR" Makefile') do set VMAJ=%%v
for /f "tokens=3" %%v in ('findstr /b "VERSION_MINOR" Makefile') do set VMIN=%%v
for /f "tokens=3" %%v in ('findstr /b "VERSION_MICRO" Makefile') do set VMIC=%%v
"%MR%" -f cia -o 3dshub.cia -elf 3dshub.elf -rsf 3dshub.rsf -icon build\icon.icn -banner build\banner.bnr -exefslogo -target t -major %VMAJ% -minor %VMIN% -micro %VMIC% || exit /b 1
echo.
echo Pronto: 3dshub.cia
