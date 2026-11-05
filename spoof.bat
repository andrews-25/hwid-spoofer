@echo off
rem HWID spoofer launcher. Passes any flags (-Restore / -DryRun) through.
rem Without flags it randomizes your MAC addresses + MachineGuid.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0spoof.ps1" %*
pause
