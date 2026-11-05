<#
  HwidSpoofer - user-mode (no kernel driver) HWID spoofer for EAC-style games.
  Spoofs: NIC MAC addresses + Windows MachineGuid.
  It NEVER starts, stops, installs, or touches EasyAntiCheat or your game.
  Reversible: original values are backed up first; -Restore puts them back.
  Cannot fake firmware serials (board/BIOS/disk) - those need a kernel driver.

  Double-click spoof.bat           = random new spoof
  spoof.bat -Restore               = put original values back
  spoof.bat -DryRun                = show what would change, change nothing
  spoof.ps1 -SelfTest              = sandbox test, touches nothing real
#>
param([switch]$Restore, [switch]$DryRun, [switch]$SelfTest)

$ErrorActionPreference = 'Stop'
$AppDir     = Join-Path $env:LOCALAPPDATA 'HwidSpoofer'
$BackupFile = Join-Path $AppDir 'backup.json'
$GuidRegKey = 'HKLM:\SOFTWARE\Microsoft\Cryptography'

function New-RandomMac {
  # first octet 02/06/0A/0E = "locally administered" range,
  # so it can never clash with a factory-assigned vendor MAC
  $base   = @(2, 6, 10, 14)[(Get-Random -Maximum 4)]
  $octets = @($base) + (1..5 | ForEach-Object { Get-Random -Maximum 256 })
  (($octets | ForEach-Object { '{0:X2}' -f $_ }) -join '-')
}

function New-RandomGuid { [guid]::NewGuid().ToString('D') }

function Require-Admin {
  $p = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent())
  if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $flags = ''
    if ($Restore) { $flags += ' -Restore' }
    if ($DryRun)  { $flags += ' -DryRun' }
    Write-Host 'Requesting administrator rights (UAC prompt)...'
    Start-Process powershell -Verb RunAs -ArgumentList @(
      '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath) + @($flags.Trim())
    exit
  }
}

function Read-Backup {
  if (-not (Test-Path $BackupFile)) {
    Write-Host "No backup file at $BackupFile - nothing to restore."
    Write-Host 'Run the spoofer first (double-click spoof.bat).'
    exit 1
  }
  Get-Content $BackupFile -Raw | ConvertFrom-Json
}

function Write-Backup($obj) {
  New-Item -ItemType Directory -Path $AppDir -Force | Out-Null
  ($obj | ConvertTo-Json -Depth 6) | Set-Content -Path $BackupFile -Encoding UTF8
  Write-Host "Backup written to $BackupFile"
}

function Get-CurrentMachineGuid {
  (Get-ItemProperty -Path $GuidRegKey -Name MachineGuid).MachineGuid
}

function Get-PhysicalNics {
  Get-NetAdapter -Physical | Where-Object { $_.MacAddress } | Sort-Object Name
}

# ---------- SELF TEST (sandbox in HKCU, touches no real machine value) ----------
if ($SelfTest) {
  Write-Host '[selftest] checking 200 random MACs + GUIDs...'
  $badMac = @(); $badGuid = @()
  1..200 | ForEach-Object {
    $m = New-RandomMac
    if ($m -notmatch '^(02|06|0A|0E)(-[0-9A-F]{2}){5}$') { $badMac += $m }
    $g = New-RandomGuid
    if ($g -notmatch '^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$') { $badGuid += $g }
  }
  Write-Host ('[selftest] MAC format failures: ' + $badMac.Count)
  Write-Host ('[selftest] GUID format failures: ' + $badGuid.Count)

  # mock-registry round trip: set original -> spoof -> verify -> restore -> verify
  $testRoot = 'HKCU:\Software\HwidSpooferTest'
  Remove-Item -Recurse -Force $testRoot -ErrorAction SilentlyContinue
  New-Item -Path $testRoot -ItemType Directory -Force | Out-Null
  $fakeOriginal = '{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}'
  Set-ItemProperty -Path $testRoot -Name MachineGuid -Value $fakeOriginal
  $spoof = New-RandomGuid
  Set-ItemProperty -Path $testRoot -Name MachineGuid -Value $spoof
  $afterSpoof = (Get-ItemProperty -Path $testRoot -Name MachineGuid).MachineGuid
  $spoofed = ($afterSpoof -eq $spoof)
  Set-ItemProperty -Path $testRoot -Name MachineGuid -Value $fakeOriginal
  $afterRestore = (Get-ItemProperty -Path $testRoot -Name MachineGuid).MachineGuid
  $restoreOk = ($afterRestore -eq $fakeOriginal)
  Remove-Item -Recurse -Force $testRoot -ErrorAction SilentlyContinue

  $p1 = @('FAIL', 'PASS')[[int]$spoofed]
  $p2 = @('FAIL', 'PASS')[[int]$restoreOk]
  Write-Host ('[selftest] mock spoof write: ' + $p1)
  Write-Host ('[selftest] mock restore round-trip: ' + $p2)
  if ($badMac.Count -eq 0 -and $badGuid.Count -eq 0 -and $spoofed -and $restoreOk) {
    Write-Host '[selftest] ALL TESTS PASSED'; exit 0
  }
  Write-Host '[selftest] TESTS FAILED'; exit 1
}

# ---------- RESTORE ----------
if ($Restore) {
  Require-Admin
  $bk = Read-Backup
  Write-Host 'Restoring original values...'
  Set-ItemProperty -Path $GuidRegKey -Name MachineGuid -Value $bk.MachineGuid
  Write-Host ('  MachineGuid restored to ' + $bk.MachineGuid)
  foreach ($name in @($bk.Adapters.PSObject.Properties.Name)) {
    $old = $bk.Adapters.$name.OldMac
    if (-not (Get-NetAdapter -Name $name -ErrorAction SilentlyContinue)) {
      Write-Host ("  NIC '$name' is gone (hardware changed) - backup MAC '$old' skipped.")
      continue
    }
    try {
      Set-NetAdapter -Name $name -MacAddress $old -Confirm:$false
      Write-Host ("  NIC '$name' MAC restored to $old")
    } catch {
      Write-Host ("  NIC '$name' driver refused the MAC change: " + $_.Exception.Message)
    }
  }
  Write-Host 'Restore complete. Reboot so every app sees the NIC changes official.'
  Write-Host 'The tool is finished - it does not sit in the background.'
  exit 0
}

# ---------- SPOOF (or DRY-RUN) ----------
if (-not $DryRun) { Require-Admin }

Write-Host '=== HWID SPOOFER (user-mode: registry + NIC MAC only) ==='
Write-Host 'It does NOT start, stop, or touch EasyAntiCheat or your game.'
Write-Host ''

$nics   = Get-PhysicalNics
$oldGuid = Get-CurrentMachineGuid
# MachineGuid on Windows 10/11 is stored WITHOUT braces; mirror whatever the
# original looked like so apps/EAC keep parsing the same format.
$newGuid = [guid]::NewGuid().ToString('D')
if ($oldGuid -match '^\{') { $newGuid = '{' + $newGuid + '}' }

if ($nics.Count -eq 0) { Write-Host 'WARNING: no physical network adapters found.' }
Write-Host ('Network adapters found: ' + $nics.Count)
foreach ($n in $nics) {
  $newMac = New-RandomMac
  Write-Host ('  NIC "' + $n.Name + '" : ' + $n.MacAddress + '  ->  ' + $newMac)
  if (-not $DryRun) {
    try {
      Set-NetAdapter -Name $n.Name -MacAddress $newMac -Confirm:$false
      # cycle the adapter so the driver reloads and reports the new MAC to Windows
      if ($n.Status -eq 'Up') {
        Disable-NetAdapter -Name $n.Name -Confirm:$false -NoWait
        Start-Sleep -Seconds 2
        Enable-NetAdapter -Name $n.Name -Confirm:$false
        Start-Sleep -Seconds 2
      }
      $shown = (Get-NetAdapter -Name $n.Name).MacAddress
      if ($shown -eq $newMac) {
        Write-Host ('    applied and verified: ' + $shown)
      } else {
        Write-Host ('    driver still reports ' + $shown + ' - REBOOT needed for this NIC.')
      }
    } catch {
      Write-Host ('    FAILED: driver refused - ' + $_.Exception.Message)
    }
  }
}

Write-Host ''
Write-Host ('MachineGuid : ' + $oldGuid + '  ->  ' + $newGuid)
if (-not $DryRun) {
  # back up ORIGINALS before writing anything new
  $backup = @{ MachineGuid = $oldGuid; Adapters = @{} }
  foreach ($n in $nics) { $backup.Adapters[$n.Name] = @{ OldMac = $n.MacAddress } }
  Write-Backup $backup
  Set-ItemProperty -Path $GuidRegKey -Name MachineGuid -Value $newGuid
  Write-Host '  MachineGuid written.'
}

Write-Host ''
if ($DryRun) {
  Write-Host '[DRY RUN] Nothing was changed. Run spoof.bat without -DryRun to apply.'
  exit 0
}
Write-Host 'Done. Close this window and launch the game normally -'
Write-Host 'the spoofer is finished and is NOT running in the background.'
Write-Host 'If any NIC says "reboot needed", reboot before playing.'
