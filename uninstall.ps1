# uninstall.ps1 - removes the HWID driver and turns TESTSIGNING back off.
# Reboot afterwards. Run as the same admin user who installed it.
$ErrorActionPreference = 'SilentlyContinue'
sc.exe stop HwidSpoof
sc.exe delete HwidSpoof
Remove-Item -Force (Join-Path $env:windir 'System32\drivers\hwid_spoof.sys')
# Delete the self-signed cert so it stops trusting our test driver:
certutil -delstore Root 'HwidSpoof Test' | Out-Null
bcdedit /set TESTSIGNING OFF | Out-Null
Write-Host 'Driver removed, TESTSIGNING off. REBOOT to finish.'
