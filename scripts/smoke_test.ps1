# Two-instance smoke test with pass/fail checks (no human needed). Uses the fast-start save (see README).
# usage: smoke_test.ps1 [-Save coop_start] [-Wait 105] [-Respawn] [-Launcher]
# -Launcher: start both through MetroCoopLauncher.exe; the DLL hosts/joins by itself (mode in the ini). The client
# doesn't walk then (no check needs it).
# -Respawn (needs an armed save, e.g. -Save coop_l19): the host also shoots the client's body and the checks include
# the automatic respawn. Needs $env:METRO_HOST_DIR / $env:METRO_CLIENT_DIR like run_test.ps1. Exit code 0 = all passed.
param([string]$Save = 'coop_start', [int]$Wait = 105, [switch]$Respawn, [switch]$Launcher)
$ErrorActionPreference = 'Stop'
$start = Get-Date
$boot = "@wait_log playing video 30;@wait 1;@pad 1000 0 0 200;@wait 2;@pad 0010 0 0 200;@wait 2;@pad 1000 0 0 200;@wait 2;" +
        "gameload $Save;@wait_log load save bin 30;@wait 1;@finish_load;@wait 1;@unpause_all;@wait 4"
# host strafes 2 s about 40 s after hosting; the client (its body arrives ~15 s after joining) walks back 2 s later
# -Respawn: ~25 s after the strafe the host turns to slot 1's body and fires 3 s; the host respawns it 5 s after death
$kill = if ($Respawn) { ";@wait 20;@aim s1 3000;@wait 4;@pad 0 0 0 3000 0 0 0 255" } else { "" }
if ($Respawn -and $Wait -lt 150) { $Wait = 150 }
if ($Launcher) {  # the launcher sets mode / join_address / engine_bind_port; the DLL inserts @host after the load
    & "$PSScriptRoot\set_ini.ps1" host "exec_on_start=$boot;@wait 55;@pad 0 -32000 0 2000$kill;@wait 60" exec_delay_s=0 | Out-Null
    # client presses A / START / A at the title screen like a player; the menu then starts and the DLL joins
    & "$PSScriptRoot\set_ini.ps1" client "exec_on_start=@wait_log playing video 30;@wait 1;@pad 1000 0 0 200;@wait 2;@pad 0010 0 0 200;@wait 2;@pad 1000 0 0 200" exec_delay_s=0 | Out-Null
} else {
    & "$PSScriptRoot\set_ini.ps1" host "exec_on_start=$boot;@host;@wait 40;@pad 0 -32000 0 2000$kill;@wait 60" exec_delay_s=0 mode= | Out-Null
    & "$PSScriptRoot\set_ini.ps1" client "exec_on_start=@wait_log playing video 30;@wait 20;@join 127.0.0.1;@wait 25;@pad 0 0 -32000 2000;@wait 60" exec_delay_s=0 engine_bind_port=1001 mode= | Out-Null
}
& "$PSScriptRoot\run_test.ps1" -Client -Launcher:$Launcher -Wait $Wait -Tail 0 | Out-Null

function LogDir($install) { "$install\metro_coop" }  # mod folder: DLL, launcher, ini, logs
function Slots($line) {  # heartbeat "sN=<id>/<state>/(x,y,z)" -> @{N = @{id; st; x; y; z}}
    $r = @{}
    foreach ($m in [regex]::Matches($line, 's(\d)=([0-9A-F]+)/(\d)/\(([-\d.]+),([-\d.]+),([-\d.]+)\)')) {
        $r[[int]$m.Groups[1].Value] = @{ id = $m.Groups[2].Value; st = [int]$m.Groups[3].Value
            x = [double]$m.Groups[4].Value; y = [double]$m.Groups[5].Value; z = [double]$m.Groups[6].Value }
    }
    $r
}
function Beats($dir) { @(Select-String "$dir\metro_coop.log" -Pattern 'heartbeat: .*s1=' | ForEach-Object Line) }
function Near($a, $b) { $a -and $b -and [math]::Abs($a.x - $b.x) -lt 0.3 -and [math]::Abs($a.z - $b.z) -lt 0.3 }

$hd = LogDir $env:METRO_HOST_DIR; $cd = LogDir $env:METRO_CLIENT_DIR
$hb = Beats $hd; $cb = Beats $cd
$hl = if ($hb.Count) { Slots $hb[-1] } else { @{} }
$cl = if ($cb.Count) { Slots $cb[-1] } else { @{} }
$c0 = if ($cb.Count) { Slots $cb[0] } else { @{} }
$alive = @(Get-Process metro -ErrorAction SilentlyContinue | Where-Object {
    $_.Path -in @("$env:METRO_HOST_DIR\metro.exe", "$env:METRO_CLIENT_DIR\metro.exe") }).Count
$crashes = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $start } -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -match 'metro\.exe' }).Count
$checks = [ordered]@{
    'both instances alive'                  = $alive -eq 2
    'no crash (event 1000)'                 = $crashes -eq 0
    'host spawned a body for slot 1'        = [bool](Select-String "$hd\metro_coop.log" -Pattern 'spawn_player \d+ entities, root new id' -Quiet)
    'slot 1 linked on both sides'           = $hl[1].st -eq 5 -and $cl[1].st -eq 5 -and $hl[1].id -eq $cl[1].id
    'client controls its body'              = $cb.Count -and $cb[-1] -match "ctl=$($cl[1].id)@"
    'host moved and the client saw it'      = $c0[0] -and $cl[0] -and [math]::Abs($cl[0].z - $c0[0].z) + [math]::Abs($cl[0].x - $c0[0].x) -gt 0.5
    'slot 0 position equal on both sides'   = Near $hl[0] $cl[0]
    'slot 1 position equal on both sides'   = Near $hl[1] $cl[1]
}
if ($Respawn) {
    $eids = @(Select-String "$cd\engine.log" -Pattern '\[net\] pid==1, eid==(\d+)' | ForEach-Object { [int]$_.Matches[0].Groups[1].Value })
    $checks['host saw the body die and respawned it'] = [bool](Select-String "$hd\metro_coop.log" -Pattern 'is dead, respawning' -Quiet)
    $checks['client got a second body'] = $eids.Count -ge 2 -and $eids[-1] -ne $eids[0] -and
        $cl[1].id -eq ('{0:X}' -f $eids[-1])
}
$fail = 0
foreach ($k in $checks.Keys) { $ok = [bool]$checks[$k]; if (-not $ok) { $fail++ }; "{0}  {1}" -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $k }
"host  last: $(if ($hb.Count) { $hb[-1] -replace '.*(s0=.*?) net=.*', '$1' })"
"client last: $(if ($cb.Count) { $cb[-1] -replace '.*(s0=.*?) net=.*', '$1' })"
"RESULT: $(if ($fail) { "FAIL ($fail of $($checks.Count))" } else { "PASS ($($checks.Count) checks)" })"
exit $fail
