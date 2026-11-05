# HWID Spoofer (user-mode, no kernel driver)

A simple, reversible HWID spoofer for games using EasyAntiCheat (EAC)-style
anti-cheats. Written in plain PowerShell so it runs on any Windows PC with
nothing to install. **It never launches, stops, or touches the anti-cheat or
the game — it just changes the identity numbers Windows itself stores.**

## What it spoofs
| Identity value | How it is changed |
|---|---|
| Network adapter MAC addresses | `Set-NetAdapter` (writes the NIC driver registry setting, then cycles the adapter so Windows/WMI reports the new MAC) |
| Windows `MachineGuid` | registry key `HKLM:\SOFTWARE\Microsoft\Cryptography` |

Random MACs use the *locally-administered* range (`02/06/0A/0E` first octet),
so they can never clash with a real factory MAC.

## What it CANNOT fake
Board serial, BIOS serial and disk SMART serials live in firmware. Only a
kernel driver or hardware tool can change those. This tool fakes the
registry/MAC layer only — it will not defeat every HWID ban on its own.

## How to use
1. **Double-click `spoof.bat`** (accept the UAC admin prompt) → new random
   spoof. Originals are saved first to
   `%LOCALAPPDATA%\HwidSpoofer\backup.json`.
2. Launch the game normally afterwards — the spoofer is done; it does **not**
   run in the background while you play.
3. If a NIC prints "REBOOT needed", reboot before playing.
4. To go back: double-click `restore.bat` (same as `spoof.bat -Restore`).

Options:
- `spoof.bat -DryRun` — show what would change, change nothing.
- `spoof.bat -Restore` — put the original values back.
- `spoof.ps1 -SelfTest` — sandbox test in `HKCU`, touches nothing real.

## Safety
- Originals are always backed up before any write; `-Restore` is exact.
- Changing `MachineGuid` is minor but can affect a few apps that key
  themselves to it; restore it if anything behaves odd.
- NIC cycling briefly drops internet during the spoof step.
