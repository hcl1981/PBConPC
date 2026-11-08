# pbc-usb-attach.ps1 -- Windows counterpart of tools/pbc-usb-attach: makes the emulated PicoBoy's
# USB device a real USB device of this PC (a COM port for PicoBoy Studio, Thonny, mpremote or the
# Arduino IDE; a drive for the firmware's USB drive mode), through usbip-win2
# (https://github.com/vadimgrn/usbip-win2, install it once).
#
# PBConPC.exe runs it as administrator (one UAC prompt) when "Connect USB to this PC" is switched
# on, with the port, its own process id and a flag file: it keeps the device attached -- again
# after every firmware restart, which unplugs it -- while PBConPC runs and the flag file exists,
# and detaches it then. From an administrator PowerShell without arguments: attach once.
param([string]$Port = "3240", [int]$OwnerPid = 0, [string]$Flag = "")

$usbip = (Get-Command usbip.exe -ErrorAction SilentlyContinue).Source
if (-not $usbip) { $usbip = Join-Path $env:ProgramFiles 'USBip\usbip.exe' }
if (-not (Test-Path $usbip)) {
    Write-Error "usbip-win2 is not installed: https://github.com/vadimgrn/usbip-win2/releases"
    exit 1
}

# "usbip port" lists imported devices as "Port 01: ..." followed by "... usbip://HOST:PORT/BUSID".
function Ours {
    $p = $null
    & $usbip port 2>$null | ForEach-Object {
        if ($_ -match '^Port (\d+):') { $p = $Matches[1] }
        if ($_ -like "*usbip://127.0.0.1:$Port/*") { $p }
    }
}

$tries = 0
while ($true) {
    if ($OwnerPid -and -not (Get-Process -Id $OwnerPid -ErrorAction SilentlyContinue)) { break }
    if ($Flag -and -not (Test-Path $Flag)) { break }
    if (-not (Ours)) { & $usbip --tcp-port $Port attach -r 127.0.0.1 -b 1-1 *> $null }
    if (-not $OwnerPid) {
        if (Ours) { "attached"; exit 0 }
        $tries++
        if ($tries -ge 10) { Write-Error "nothing to attach: is PBConPC running with --usbip $Port?"; exit 1 }
    }
    Start-Sleep -Seconds 2
}
Ours | ForEach-Object { & $usbip detach -p $_ *> $null }
