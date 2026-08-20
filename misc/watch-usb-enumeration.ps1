<#
.SYNOPSIS
    Watch how Windows enumerates DeskHop, and log every change with a timestamp.

.DESCRIPTION
    Entering config mode resets a board, so it drops off USB and re-attaches a moment later. This
    records what the host actually saw: the detach, whether the board came back, and how long the
    gap was. It is the tool behind the topology note in the README - behind two cascaded hubs the
    board took 15.0 s, 15.9 s and 23.9 s to reappear; on a motherboard root port, 815 ms.

    Run it, trigger the transition, and read the gap. Under a second is healthy. Tens of seconds
    means the hub is not reporting the re-attach upstream, and the board wants a root port.

    Ctrl+C does not stop this script, on purpose. The config-mode hotkey is Left Ctrl + Right Shift
    + C + O, and the combination is not matched until O arrives - so Ctrl+C reaches Windows first
    and would kill an ordinary script the instant you triggered the very thing being measured.
    Press Q or Escape to quit instead.

    Everything is written to the log file as well as the console, so a console that dies mid-run
    loses nothing.

.PARAMETER VidPid
    Devices to watch, as VID:PID strings. Defaults to DeskHop in both its modes. The retired
    2E8A:107C is still watched so the script stays useful against older firmware.

.PARAMETER IntervalMs
    Poll interval, default 100 ms. A detach shorter than this can slip between samples; drop it to
    50 before concluding the host never saw one.

.PARAMETER LogFile
    Defaults to deskhop-usb-watch.log in the user profile.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File misc\watch-usb-enumeration.ps1
    Watch DeskHop, then press the config hotkey and read the gap.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File misc\watch-usb-enumeration.ps1 -VidPid 3434:D028
    Watch some other device instead - useful for testing whether a candidate hub drops re-attaches.
#>

[CmdletBinding()]
param(
    [string[]] $VidPid     = @('1209:C000', '2E8A:107C'),
    [int]      $IntervalMs = 100,
    [string]   $LogFile    = "$env:USERPROFILE\deskhop-usb-watch.log"
)

$ErrorActionPreference = 'SilentlyContinue'
$ProgressPreference    = 'SilentlyContinue'

# -File passes "a,b" as one string, -Command passes it as an array. Handle both.
$watch = @($VidPid | ForEach-Object { $_ -split ',' } |
    ForEach-Object { $_.Trim().ToUpper() } | Where-Object { $_ })

function Say {
    param([string] $Text, [string] $Colour = 'Gray')
    Write-Host $Text -ForegroundColor $Colour
    try { Add-Content -Path $LogFile -Value $Text -Encoding UTF8 } catch { }
}

function Snapshot {
    $d = Get-PnpDevice | Where-Object { $_.Status -eq 'OK' }
    $rows = @()
    foreach ($x in @($d)) {
        if ($x.InstanceId -notmatch 'VID_([0-9A-Fa-f]{4})&PID_([0-9A-Fa-f]{4})') { continue }
        $key = '{0}:{1}' -f $matches[1].ToUpper(), $matches[2].ToUpper()
        if ($watch -notcontains $key) { continue }
        $rows += '{0}|{1}' -f $x.Class,
                 ($x.InstanceId -replace '^.*(VID_[0-9A-F]{4}&PID_[0-9A-F]{4})', '$1')
    }
    return $rows | Sort-Object
}

# Keep Ctrl+C from killing the run - see the note in the help above.
try { [Console]::TreatControlCAsInput = $true } catch { }

function ShouldQuit {
    try {
        while ([Console]::KeyAvailable) {
            $k = [Console]::ReadKey($true)
            if ($k.Key -eq 'Q' -or $k.Key -eq 'Escape') { return $true }
        }
    } catch { }
    return $false
}

Say ""
Say ("=== usb enumeration watch started {0:yyyy-MM-dd HH:mm:ss} ===" -f (Get-Date)) 'Cyan'
Say ("watching: {0}" -f ($watch -join ', ')) 'DarkGray'
Say "log: $LogFile" 'DarkGray'

$prev       = @(Snapshot)
$start      = Get-Date
$lastChange = $start
$lastBeat   = $start

Say ("[{0:HH:mm:ss.fff}] baseline: {1} node(s)" -f $start, $prev.Count) 'Cyan'
$prev | ForEach-Object { Say "    $_" 'DarkGray' }
Say "Watching - trigger the transition now. Q or Escape to quit (Ctrl+C will not stop this)." 'Yellow'

while (-not (ShouldQuit)) {
    try {
        Start-Sleep -Milliseconds $IntervalMs
        $now = @(Snapshot)
        $t   = Get-Date

        $added   = @($now  | Where-Object { $prev -notcontains $_ })
        $removed = @($prev | Where-Object { $now  -notcontains $_ })

        if ($added.Count -or $removed.Count) {
            Say ("[{0:HH:mm:ss.fff}] +{1,8:N0} ms   now {2} node(s)" -f `
                 $t, ($t - $lastChange).TotalMilliseconds, $now.Count) 'Cyan'
            $removed | ForEach-Object { Say "    GONE    $_" 'Red' }
            $added   | ForEach-Object { Say "    APPEAR  $_" 'Green' }
            if ($now.Count -eq 0) { Say "    -- fully detached; host sees nothing --" 'Yellow' }
            $prev = $now; $lastChange = $t; $lastBeat = $t
        }
        elseif (($t - $lastBeat).TotalSeconds -ge 15) {
            Say ("[{0:HH:mm:ss.fff}] .. alive, {1} node(s), {2:N0}s since last change" -f `
                 $t, $now.Count, ($t - $lastChange).TotalSeconds) 'DarkGray'
            $lastBeat = $t
        }
    }
    catch {
        Say ("[{0:HH:mm:ss.fff}] ERROR: {1}" -f (Get-Date), $_.Exception.Message) 'Magenta'
        Start-Sleep -Milliseconds 500
    }
}

Say ("=== stopped {0:HH:mm:ss} ===" -f (Get-Date)) 'Cyan'
