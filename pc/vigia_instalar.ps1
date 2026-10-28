# Cria (ou atualiza) a tarefa "3DS Hub - vigia": ao entrar no Windows e a cada
# 2 minutos ela confere se o Hub do PC esta aberto e, se nao estiver, abre de novo.
# Chamado pelo instalar.bat. Para desligar: Agendador de Tarefas > "3DS Hub - vigia".
$ErrorActionPreference = "Stop"
$pasta = $PSScriptRoot
$vigia = Join-Path $pasta "vigia.vbs"

# o pythonw.exe do Python que o "py -3" usa (sem janela)
$pyw = & py -3 -c "import os, sys; print(os.path.join(os.path.dirname(sys.executable), 'pythonw.exe'))"
if (-not $pyw -or -not (Test-Path $pyw)) { $pyw = "pyw.exe" }

$usuario = "$env:USERDOMAIN\$env:USERNAME"
$acao = New-ScheduledTaskAction -Execute "$env:WINDIR\System32\wscript.exe" `
    -Argument "//B //Nologo `"$vigia`" `"$pyw`""
$aoEntrar = New-ScheduledTaskTrigger -AtLogOn -User $usuario
$aCada2Min = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) -RepetitionInterval (New-TimeSpan -Minutes 2)
$ajustes = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
    -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 1)
$quem = New-ScheduledTaskPrincipal -UserId $usuario -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName "3DS Hub - vigia" -Force `
    -Description "Reabre o 3DS Hub do PC se ele fechar ($vigia)" `
    -Action $acao -Trigger $aoEntrar, $aCada2Min -Settings $ajustes -Principal $quem | Out-Null
Write-Host "Vigia ligado: se o Hub fechar, ele abre de novo sozinho em ate 2 minutos."

# fecha o Hub que estiver rodando: o instalar.bat abre em seguida a versao desta pasta
Get-CimInstance Win32_Process -Filter "Name like 'py%'" |
    Where-Object { $_.CommandLine -like '*hub.py*' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
