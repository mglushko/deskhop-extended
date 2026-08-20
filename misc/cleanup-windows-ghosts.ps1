<#
.SYNOPSIS
    Remove non-present ("ghost") device nodes left behind by removable hardware on Windows.

.DESCRIPTION
    Windows keeps a permanent device node for every device identity it has ever seen. A device that
    reports a USB serial number is keyed on VID+PID+serial and so keeps one node forever. A device
    that reports no serial is keyed on its hub port path instead, so every different port or hub
    position mints another identity - and the old one lingers as a hidden, non-present node. Over
    time Device Manager fills with duplicate "HID Keyboard Device", "USB Composite Device" and
    "USB Mass Storage Device" entries.

    Plugging in storage leaves a second trail, under different enumerators and with no VID/PID in
    the instance ID at all: Disk drives (USBSTOR), Portable Devices (SWD\WPDBUSENUM), Storage
    volumes and Storage volume shadow copies (STORAGE). Those are covered here too.

    This script lists those non-present nodes and, with -Remove, deletes them. Present (connected)
    devices are never touched.

    DeskHop itself is a small part of any such pile-up: it reports a per-board serial derived from
    the RP2040 flash unique ID, so it holds one identity per physical board.

.PARAMETER Remove
    Actually delete the nodes. Without this the script only reports (dry run). Requires an elevated
    shell.

.PARAMETER VidPid
    Restrict to specific devices, as VID:PID strings, e.g. -VidPid 1209:C000,2E8A:107C.
    Only matches nodes that carry a VID/PID, so it excludes the storage enumerators.

.PARAMETER All
    Widen the scope to every non-present node, including software devices, network adapters and
    display/PCI entries. Off by default: those are not hardware plug-in leftovers and are better
    reviewed by hand.

.PARAMETER Passes
    How many removal passes to make (default 3). Removing a parent can expose orphaned children
    that only become removable afterwards, so repeat passes catch what the previous one left.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File misc\cleanup-windows-ghosts.ps1
    Dry run: show what would be removed, grouped by device.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File misc\cleanup-windows-ghosts.ps1 -Remove
    Remove every removable-hardware ghost. Run from an elevated shell.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File misc\cleanup-windows-ghosts.ps1 -VidPid 1209:C000,2E8A:107C -Remove
    Remove only DeskHop's ghosts.
#>

[CmdletBinding()]
param(
    [switch] $Remove,
    [string[]] $VidPid,
    [switch] $All,
    [int] $Passes = 3
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Enumerators that removable hardware leaves nodes under. Anything else (SWD software devices,
# Net adapters, PCI, DISPLAY) is left alone unless -All is given.
$RemovableEnumerators = @(
    '^USB\\'
    '^HID\\'
    '^USBSTOR\\'
    '^SWD\\WPDBUSENUM\\'
    '^STORAGE\\VOLUME'
)

function Get-DeviceKey {
    param([string] $InstanceId)

    # Preferred: the USB vendor/product pair.
    if ($InstanceId -match 'VID_([0-9A-Fa-f]{4})&PID_([0-9A-Fa-f]{4})') {
        return ('{0}:{1}' -f $matches[1].ToUpper(), $matches[2].ToUpper())
    }
    # Storage enumerators identify by vendor/product strings instead.
    if ($InstanceId -match 'VEN_([^&#\\]+)&PROD_([^&#\\]+)') {
        return ('{0} {1}' -f $matches[1], $matches[2])
    }
    # Fall back to the enumerator itself, e.g. STORAGE\VOLUMESNAPSHOT.
    $parts = $InstanceId -split '\\'
    if ($parts.Count -ge 2) { return ('{0}\{1}' -f $parts[0], $parts[1]) }
    return $parts[0]
}

function Get-VidPid {
    param([string] $InstanceId)
    if ($InstanceId -match 'VID_([0-9A-Fa-f]{4})&PID_([0-9A-Fa-f]{4})') {
        return ('{0}:{1}' -f $matches[1].ToUpper(), $matches[2].ToUpper())
    }
    return $null
}

function Get-GhostDevices {
    param([string[]] $Filter, [switch] $Wide)

    # Status 'Unknown' is how Get-PnpDevice reports a node whose device is not currently attached.
    # This is the single guard that keeps connected hardware safe - do not relax it.
    $ghosts = Get-PnpDevice -Status Unknown -ErrorAction SilentlyContinue

    $result = @()
    foreach ($d in @($ghosts)) {
        $id = $d.InstanceId

        if (-not $Wide) {
            $inScope = $false
            foreach ($rx in $RemovableEnumerators) {
                if ($id -match $rx) { $inScope = $true; break }
            }
            if (-not $inScope) { continue }
        }

        if ($Filter) {
            $vp = Get-VidPid $id
            if (-not $vp -or ($Filter -notcontains $vp)) { continue }
        }

        $result += [pscustomobject]@{
            Key          = Get-DeviceKey $id
            Class        = $d.Class
            FriendlyName = $d.FriendlyName
            InstanceId   = $id
            # Children sit below their parent, so removing deepest-first avoids orphaning nodes.
            Depth        = [regex]::Matches($id, '[&\\#]').Count
        }
    }
    return $result
}

function Test-Elevated {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

$filter = $null
if ($VidPid) {
    # -File passes "a,b" as one string, -Command passes it as an array. Handle both.
    $filter = @($VidPid | ForEach-Object { $_ -split ',' } |
        ForEach-Object { $_.Trim().ToUpper() } | Where-Object { $_ })
    Write-Host "Restricting to: $($filter -join ', ')" -ForegroundColor Cyan
}

if ($All) {
    Write-Host "Scope: every non-present node (-All)." -ForegroundColor Yellow
}

$devices = @(Get-GhostDevices -Filter $filter -Wide:$All)

if ($devices.Count -eq 0) {
    Write-Host "No non-present device nodes found. Nothing to do." -ForegroundColor Green
    return
}

Write-Host ""
Write-Host "Non-present (ghost) device nodes: $($devices.Count)" -ForegroundColor Yellow
Write-Host ""

$devices | Group-Object Key | Sort-Object Count -Descending | ForEach-Object {
    $sample = $_.Group[0].FriendlyName
    if (-not $sample) { $sample = $_.Group[0].Class }
    '{0,5}  {1,-28} {2}' -f $_.Count, $_.Name, $sample
}

Write-Host ""
$devices | Group-Object Class | Sort-Object Count -Descending | ForEach-Object {
    $name = $_.Name
    if (-not $name) { $name = '(none)' }
    '{0,5}  class {1}' -f $_.Count, $name
}
Write-Host ""

if (-not $Remove) {
    Write-Host "Dry run - nothing was changed." -ForegroundColor Green
    Write-Host "Re-run with -Remove from an elevated shell to delete these nodes." -ForegroundColor Green
    return
}

if (-not (Test-Elevated)) {
    Write-Error "-Remove needs an elevated shell. Re-run PowerShell as Administrator."
    return
}

$totalRemoved = 0

for ($pass = 1; $pass -le $Passes; $pass++) {
    $batch = @(Get-GhostDevices -Filter $filter -Wide:$All | Sort-Object Depth -Descending)
    if ($batch.Count -eq 0) { break }

    Write-Host "Pass $pass - $($batch.Count) node(s) to remove" -ForegroundColor Yellow
    $removedThisPass = 0

    foreach ($d in $batch) {
        # pnputil is the supported path and refuses devices that are actually present.
        $out = & pnputil.exe /remove-device $d.InstanceId 2>&1
        if ($LASTEXITCODE -eq 0) {
            $removedThisPass++
            Write-Verbose "removed $($d.InstanceId)"
        } else {
            Write-Verbose "skipped $($d.InstanceId): $out"
        }
    }

    $totalRemoved += $removedThisPass
    Write-Host "  removed $removedThisPass" -ForegroundColor Green
    if ($removedThisPass -eq 0) { break }
}

$left = @(Get-GhostDevices -Filter $filter -Wide:$All)
Write-Host ""
Write-Host "Removed $totalRemoved node(s). $($left.Count) remaining." -ForegroundColor Cyan
Write-Host "Anything remaining is usually held by a driver until the next reboot." -ForegroundColor Cyan
