# Records the debug log of the Sofle's left half (diagnostic firmware) to a text file.
#
# 1. Flash eyelash_sofle_left_DIAGNOSTIC.uf2 onto the left half and plug it in by USB.
# 2. Switch typing to Bluetooth (layer 2 + S), so the problem path is the one being used.
# 3. Run this script, type normally until a key sticks, then press Ctrl+C.
#
# It only reads from the keyboard's USB serial port and writes a log file next to this
# script; it sends nothing to the keyboard and makes no network connections.

$ErrorActionPreference = 'Stop'

$ports = Get-CimInstance Win32_PnPEntity |
    Where-Object { $_.PNPDeviceID -match 'VID_1D50&PID_615E' -and $_.Name -match '\((COM\d+)\)' } |
    ForEach-Object { [regex]::Match($_.Name, 'COM\d+').Value }
if (-not $ports) {
    Write-Host "No keyboard serial port found. Is the left half plugged in by USB with the diagnostic firmware?"
    exit 1
}

# The diagnostic firmware has two serial ports: ZMK Studio's (silent unless Studio talks to it)
# and the log's. Pick the one that sends text; press a key or two while it looks.
Write-Host "Looking for the log port among: $($ports -join ', ') - press a few keys..."
$serial = $null
foreach ($p in $ports) {
    $s = New-Object System.IO.Ports.SerialPort $p, 115200
    $s.DtrEnable = $true   # the keyboard only sends while the port is "open" on this side
    $s.ReadTimeout = 500
    try { $s.Open() } catch { continue }
    $deadline = (Get-Date).AddSeconds(5)
    while ((Get-Date) -lt $deadline -and $s.BytesToRead -eq 0) { Start-Sleep -Milliseconds 200 }
    if ($s.BytesToRead -gt 0) { $serial = $s; $port = $p; break }
    $s.Close()
}
if (-not $serial) {
    Write-Host "None of the keyboard's ports sent log text. Is the DIAGNOSTIC firmware on the left half?"
    exit 1
}

$logFile = Join-Path $PSScriptRoot ("keyboard_log_{0:yyyy-MM-dd_HH-mm-ss}.txt" -f (Get-Date))
Write-Host "Recording $port to $logFile - press Ctrl+C to stop."

$writer = New-Object System.IO.StreamWriter $logFile, $true
try {
    while ($true) {
        try {
            $line = $serial.ReadLine().TrimEnd("`r")
            $stamped = "{0:HH:mm:ss.fff}  {1}" -f (Get-Date), $line
            $writer.WriteLine($stamped)
            $writer.Flush()
            if ($line -match '<wrn>|<err>') { Write-Host $stamped -ForegroundColor Yellow }
        } catch [System.TimeoutException] { }
    }
} finally {
    $writer.Close()
    $serial.Close()
    Write-Host "Saved $logFile"
}
