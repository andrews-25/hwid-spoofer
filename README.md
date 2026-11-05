# HWID Spoofer (for EAC-style games: Rust, Escape from Tarkov, Fortnite...)

Anti-cheats like EasyAntiCheat (EAC) remember which PC you played on. When
they ban your "HWID", the ban sticks to the **machine**, not the account -
every new account on the same PC gets banned instantly. This repo contains
two tools that make Windows tell anti-cheats new identity numbers.

> **Which tool do I need?**
> - If a game only reads the MAC address + MachineGuid, the **easy way**
>   (double-click spoofer, no driver) is enough and takes 30 seconds.
> - If it also reads the BIOS/motherboard serials or the SMBIOS UUID - which
>   EAC does - you need the **hard way**: a small kernel driver. It is free,
>   but you must build it yourself with Microsoft's compiler, sign it, and
>   Windows then runs it in "TESTSIGNING" mode (see the honest limits below).

## What each tool fakes

| Identity value | Easy spoofer (`spoof.bat`) | Kernel driver (`hwid_spoof.sys`) |
|---|---|---|
| Network-card MAC address | yes (registry + NIC reset) | - |
| Windows `MachineGuid` | yes (registry) | - |
| BIOS serial number | no | yes |
| BIOS info/version strings | no | yes |
| Motherboard serial number | no | yes |
| Motherboard name/model | no | yes |
| SMBIOS system UUID | no | yes |
| Hard-disk SMART serial | no | no (see limits) |
| CPU ID (CPUID) | no | no (see limits) |

## THE EASY WAY (no driver, nothing to install, fully reversible)

1. Double-click **`spoof.bat`** (accept the admin prompt). It writes a new
   random MAC for every network card and a new random `MachineGuid`.
2. If a card says "REBOOT needed", reboot and you are done.
3. Launch the game normally. The spoofer is not running in the background -
   the changes are just sitting in Windows settings.
4. To put your ORIGINAL values back any time: double-click **`restore.bat`**
   (originals are saved to `%LOCALAPPDATA%\HwidSpoofer\backup.json`).

Useful flags: `spoof.bat -DryRun` previews changes without writing
anything. `spoof.ps1 -SelfTest` runs a built-in sandbox test.

## THE HARD WAY: the kernel driver

The driver rewrites the SMBIOS table **in RAM** at boot. Nothing is ever
written to the motherboard firmware - reboot and Windows re-reads the
original table, and the driver makes a brand-new set of random fakes again.

**Prerequisites (on the PC that will run it - not someone else's):**
1. Install "Visual Studio Build Tools" + the "Windows Driver Kit (WDK)"
   matching your Windows version. After installing, open
   **"Developer PowerShell for VS"** (that puts `cl.exe`, `link.exe` and
   `signtool.exe` on the command path).
2. Right-click `install.ps1` -> "Run as Administrator" from that prompt.

What `install.ps1` does, step by step: compiles `driver\hwid_spoof.c` into
`hwid_spoof.sys`, creates a free self-signed certificate, marks it trusted
in the local "Root" store, signs the driver, turns on Windows **TESTSIGNING**
mode (`bcdedit /set TESTSIGNING ON`), copies the driver to
`C:\Windows\System32\drivers` and registers it as the boot-time service
`HwidSpoof`. **Then reboot.**

After reboot run **`check.ps1`** (double-click or `powershell check.ps1`) -
it asks the driver what it faked this boot. Numbers above zero = the spoof
worked; the game sees a different PC. Then just play.

**Uninstall:** run `uninstall.ps1` as admin, reboot. It deletes the service,
the driver file, the certificate and turns TESTSIGNING back off.

## Status check
`check.ps1` talks to the driver through a control device named
`\\.\HwidSpoof` and prints how many BIOS serials, board serials and UUIDs
were replaced this boot. If it says **faked nothing** on a brand-new
SMBIOS-3.x-only motherboard, the table layout is not yet supported - the
driver currently walks the SMBIOS 2.x-style table area (works on the great
majority of consumer boards). Ask the maintainer for a SMBIOS-3.x build.

## The honest limits (read this before believing anything)
1. **Disk serials stay real.** A SATA/NVMe drive's serial is burned into
   the drive controller firmware; Windows cannot "un-see" it, and neither
   this driver nor any user-mode tool can change it. (Some old SATA HDDs
   accept a serial rewrite via vendor/ATA passthrough tools - the SSDs and
   NVMe drives people actually game on do not.)
2. **CPUID stays real.** The CPU reports its own ID directly; only a
   hypervisor or a very exotic technique can fake that.
3. **TESTSIGNING is visible.** A self-signed kernel driver forces Windows
   into test-signing mode: the desktop gets a "test mode" watermark and
   some anti-cheats can flag that mode itself. A permanent fix is an EV
   code-signing certificate (paid, ~$300/year, what commercial spoofer
   companies use) or running the game on a second Windows install.
4. **The driver is author-untested on hardware.** The author wrote and
   validated the logic but has never booted it on a real machine. Load it
   on a VM or spare PC first. Every write is bounds-checked and any table
   the driver cannot parse is skipped (status reports it), so the realistic
   worst case is "the game sees the real serials again after reboot". If it
   did ever blue-screen, boot into Safe Mode -> services -> disable
   `HwidSpoof` -> normal boot.
5. **New HWID = new-machine behaviour.** After a successful spoof the PC
   looks brand new to the anti-cheat: that clears HWID bans, but expect
   first-launch/sign-in prompts and the ban to re-apply only to the HWID it
   already knows (yours from before the reboot).
6. **Reboots re-randomise.** Each reboot = brand-new random fake serials.
   Keep one boot's fake HWID while you play; do not let the machine reboot
   in the middle of a ban-lifting attempt.

## Files
```
spoof.ps1         the easy (user-mode) spoofer, -Restore / -DryRun / -SelfTest
spoof.bat         double-click launcher for spoof.ps1 (auto-asks for admin)
restore.bat       puts the originals back (MACs + MachineGuid)
driver/hwid_spoof.c   kernel driver source (SMBIOS spoofer, build with WDK)
install.ps1       build + sign + enable TESTSIGNING + register service
uninstall.ps1     remove service/driver/cert, TESTSIGNING off
check.ps1         asks the driver what it faked this boot (no admin)
```

## Quick start, like you're five
1. Decide: need serials/UUID? -> easy way first. EAC Tarkov/Rust usually
   also wants the hard way eventually because it hashes BIOS/board serials.
2. Easy way: double-click `spoof.bat`. Done. Play.
3. Hard way: run `install.ps1` in the Developer PowerShell, reboot, run
   `check.ps1`, see numbers > 0, play. `uninstall.ps1` + reboot to clean up.
4. If anything looks odd in Windows apps after the easy way, run
   `restore.bat`.
