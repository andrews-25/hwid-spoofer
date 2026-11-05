# install.ps1 - sign, register and start hwid_spoof.sys (RUN ON YOUR OWN PC)
# Needs: WDK build tools on PATH (cl.exe, link.exe, signtool.exe) and admin.
# Reboot required after install. See README.md section "THE HARD WAY".
param([string]$Src = 'driver')
$ErrorActionPreference = 'Stop'
$dri = Join-Path $PSScriptRoot $Src
$obj = Join-Path $dri 'build'
New-Item -ItemType Directory -Force $obj | Out-Null

Write-Host '[1/4] Building the driver...'
cl /nologo /W4 /O2 /GS- /c "$dri\hwid_spoof.c" /Fo "$obj\hwid_spoof.obj"
if ($LASTEXITCODE -ne 0) { throw 'cl.exe not found - install VS Build Tools + WDK (see README).' }
link /nologo /driver /OUT:"$obj\hwid_spoof.sys" "$obj\hwid_spoof.obj" ntoskrnl.lib
if ($LASTEXITCODE -ne 0) { throw 'link failed (wrong WDK libs?).' }

Write-Host '[2/4] Self-signing (test-signing) the driver...'
$cert = New-SelfSignedCertificate -Subject 'CN=HwidSpoof Test' -Type CodeSigningCert
$cer  = Join-Path $obj 'hwidspoof.cer'
Export-Certificate -Cert $cert -FilePath $cer | Out-Null
certutil -addstore -f Root $cer | Out-Null
signtool sign /n 'HwidSpoof Test' /s LocalMachine `
        /t http://timestamp.digicert.com "$obj\hwid_spoof.sys"
if ($LASTEXITCODE -ne 0) { throw 'signtool failed.' }

Write-Host '[3/4] Enabling Windows TESTSIGNING mode + installing...'
bcdedit /set TESTSIGNING ON | Out-Null
$dst = Join-Path $env:windir 'System32\drivers\hwid_spoof.sys'
Copy-Item "$obj\hwid_spoof.sys" $dst -Force
sc.exe create HwidSpoof type= kernel binPath= $dst start= boot
sc.exe start HwidSpoof

Write-Host '[4/4] Done. REBOOT the PC now.'
Write-Host '    After reboot run check.ps1 to confirm the driver faked values.'
Write-Host '    WARNING: TESTSIGNING stays ON - EAC/anti-cheat can see that.'
Write-Host '    Run uninstall.ps1 later to remove everything.'
