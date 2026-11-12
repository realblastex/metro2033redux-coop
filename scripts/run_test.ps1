# Launch the host (and with -Client a second instance), inject the DLL, wait, then print both logs and take
# screenshots. Set each instance's exec_on_start first with set_ini.ps1.
# usage: run_test.ps1 [-Client] [-Wait 80] [-HostDir <dir>] [-ClientDir <dir>]
param([switch]$Client, [int]$Wait = 80, [int]$InjectDelay = 3, [int]$Tail = 30,
      [string]$HostDir = $env:METRO_HOST_DIR, [string]$ClientDir = $env:METRO_CLIENT_DIR,
      [string]$Build = "$PSScriptRoot\..\build\Release")
$inj = "$Build\injector.exe"; $dll = (Resolve-Path "$Build\metro_coop.dll").Path
$installs = [ordered]@{ host = $HostDir }
if ($Client) { $installs.client = $ClientDir }
foreach ($k in $installs.Keys) { if (-not $installs[$k]) { throw "no $k install folder (-HostDir/-ClientDir or METRO_*_DIR)" } }
function LogDir($install) { "$env:LOCALAPPDATA\MetroCoop\" + ((Split-Path $install -Leaf) -replace '[^A-Za-z0-9_-]', '_') }

foreach ($k in $installs.Keys) {  # stop old instances of these installs and clear their logs
    Get-Process metro -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$($installs[$k])\metro.exe" } | Stop-Process -Force
}
Start-Sleep 2
foreach ($k in $installs.Keys) { Remove-Item "$(LogDir $installs[$k])\*.log" -ErrorAction SilentlyContinue }

foreach ($k in $installs.Keys) {
    $exe = "$($installs[$k])\metro.exe"
    & $inj --launch $exe        # starts the game with the single-instance check patched in memory
    Start-Sleep $InjectDelay
    & $inj $exe $dll | Select-String 'success|fail'
}
Start-Sleep $Wait
foreach ($k in $installs.Keys) {
    $d = LogDir $installs[$k]
    "===== $k metro_coop.log"; Get-Content "$d\metro_coop.log" -ErrorAction SilentlyContinue | Select-String 'engine|exec|heartbeat|ERROR|WARN'
    "===== $k engine.log (last $Tail)"; Get-Content "$d\engine.log" -Tail $Tail -ErrorAction SilentlyContinue
}
Get-Process metro -ErrorAction SilentlyContinue | Select-Object Id, Path, Responding | Format-Table | Out-String
& "$PSScriptRoot\screenshot.ps1" "$env:TEMP"
