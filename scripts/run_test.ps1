# Launch the host (and with -Client a second instance), inject the DLL, wait, then print both logs and take
# screenshots. Set each instance's exec_on_start first with set_ini.ps1.
# -Launcher: start through MetroCoopLauncher.exe (--host; client --join 127.0.0.1 --second once the host is hosting)
# usage: run_test.ps1 [-Client] [-Launcher] [-Wait 80] [-HostDir <dir>] [-ClientDir <dir>]
param([switch]$Client, [switch]$Launcher, [int]$Wait = 80, [int]$InjectDelay = 3, [int]$Tail = 30,
      [string]$HostDir = $env:METRO_HOST_DIR, [string]$ClientDir = $env:METRO_CLIENT_DIR,
      [string]$Build = "$PSScriptRoot\..\build\Release")
$Build = (Resolve-Path $Build).Path; $inj = "$Build\injector.exe"
$installs = [ordered]@{ host = $HostDir }
if ($Client) { $installs.client = $ClientDir }
foreach ($k in $installs.Keys) { if (-not $installs[$k]) { throw "no $k install folder (-HostDir/-ClientDir or METRO_*_DIR)" } }
function LogDir($install) { "$install\metro_coop" }  # mod folder: DLL, launcher, ini, logs

foreach ($k in $installs.Keys) {  # stop old instances of these installs and clear their logs
    Get-Process metro -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$($installs[$k])\metro.exe" } | Stop-Process -Force
}
Start-Sleep 2
foreach ($k in $installs.Keys) {  # install this build into the mod folder, clear old logs
    $d = LogDir $installs[$k]
    New-Item -ItemType Directory -Force $d | Out-Null
    Copy-Item "$Build\metro_coop.dll", "$Build\MetroCoopLauncher.exe" $d -Force
    Remove-Item "$d\*.log" -ErrorAction SilentlyContinue
}

foreach ($k in $installs.Keys) {
    $exe = "$($installs[$k])\metro.exe"
    if ($Launcher) {
        if ($k -eq 'client') {  # a player joins once the host is in its level and hosting
            $hl = "$(LogDir $installs.host)\metro_coop.log"
            for ($i = 0; $i -lt 120 -and -not ((Test-Path $hl) -and (Select-String $hl -Pattern 'engine: net_host' -Quiet)); $i++) { Start-Sleep 1 }
        }
        [string[]]$la = if ($k -eq 'host') { '--host' } else { '--join', '127.0.0.1', '--second' }
        & "$(LogDir $installs[$k])\MetroCoopLauncher.exe" @la | Out-Null  # Out-Null waits for the GUI exe
        "launcher $k exit $LASTEXITCODE"
        continue
    }
    & $inj --launch $exe        # starts the game with the single-instance check patched in memory
    Start-Sleep $InjectDelay
    & $inj $exe "$(LogDir $installs[$k])\metro_coop.dll" | Select-String 'success|fail'
}
Start-Sleep $Wait
foreach ($k in $installs.Keys) {
    $d = LogDir $installs[$k]
    "===== $k metro_coop.log"; Get-Content "$d\metro_coop.log" -ErrorAction SilentlyContinue | Select-String 'engine|exec|heartbeat|ERROR|WARN'
    "===== $k engine.log (last $Tail)"; Get-Content "$d\engine.log" -Tail $Tail -ErrorAction SilentlyContinue
}
Get-Process metro -ErrorAction SilentlyContinue | Select-Object Id, Path, Responding | Format-Table | Out-String
& "$PSScriptRoot\screenshot.ps1" "$env:TEMP"
