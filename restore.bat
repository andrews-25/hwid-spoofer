@echo off
rem Puts your original MACs + MachineGuid back from the backup file.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0spoof.ps1" -Restore %*
pause
