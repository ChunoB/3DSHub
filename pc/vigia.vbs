' Vigia do 3DS Hub: se o Hub do PC nao estiver aberto, abre de novo (sem janela).
' Roda pela tarefa agendada "3DS Hub - vigia" (criada pelo instalar.bat) a cada
' 2 minutos e ao entrar no Windows.
' Uso: wscript vigia.vbs [caminho do pythonw.exe]   (sem caminho: usa o "pyw")
' Para parar de vez: Agendador de Tarefas > "3DS Hub - vigia" > Desabilitar.
Option Explicit
Dim fso, pasta, pyw
Set fso = CreateObject("Scripting.FileSystemObject")
pasta = fso.GetParentFolderName(WScript.ScriptFullName)
If WScript.Arguments.Count > 0 Then pyw = WScript.Arguments(0) Else pyw = "pyw.exe"

' o Hub ja esta aberto? (pythonw ou python rodando um hub.py)
Dim wmi, procs, p
Set wmi = GetObject("winmgmts:\\.\root\cimv2")
Set procs = wmi.ExecQuery("SELECT CommandLine FROM Win32_Process WHERE Name='pythonw.exe' OR Name='python.exe'")
For Each p In procs
    If Not IsNull(p.CommandLine) Then
        If InStr(LCase(p.CommandLine), "hub.py") > 0 Then WScript.Quit
    End If
Next

' anota no log do Hub, para saber se ele anda caindo
Dim lg
On Error Resume Next
Set lg = fso.OpenTextFile(pasta & "\hub.log", 8, True)
lg.WriteLine "Vigia: o Hub estava fechado, abrindo de novo (" & Now & ")"
lg.Close
On Error GoTo 0

Dim sh, cmd
Set sh = CreateObject("WScript.Shell")
sh.CurrentDirectory = pasta
cmd = """" & pyw & """ "
If LCase(fso.GetFileName(pyw)) = "pyw.exe" Then cmd = cmd & "-3 "
sh.Run cmd & """" & pasta & "\hub.py""", 0, False
