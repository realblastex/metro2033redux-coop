# Two-instance smoke test with pass/fail checks (no human needed). Uses the fast-start save (see README).
# usage: smoke_test.ps1 [-Save coop_start] [-Wait 105]
# Needs $env:METRO_HOST_DIR / $env:METRO_CLIENT_DIR like run_test.ps1. Exit code 0 = all checks passed.
param([string]$Save = 'coop_start', [int]$Wait = 105)
$ErrorActionPreference = 'Stop'
$start = Get-Date
$boot = "@wait_log playing video 30;@wait 1;@pad 1000 0 0 200;@wait 2;@pad 0010 0 0 200;@wait 2;@pad 1000 0 0 200;@wait 2;" +
        "gameload $Save;@wait_log load save bin 30;@wait 1;@finish_load;@wait 1;@unpause_all;@wait 4"
# host strafes 2 s about 40 s after hosting; the client (its body arrives ~15 s after joining) walks back 2 s later
& "$PSScriptRoot\set_ini.ps1" host "exec_on_start=$boot;@host;@wait 40;@pad 0 -32000 0 2000;@wait 60" exec_delay_s=0 | Out-Null
& "$PSScriptRoot\set_ini.ps1" client "exec_on_start=@wait_log playing video 30;@wait 20;@join 127.0.0.1;@wait 25;@pad 0 0 -32000 2000;@wait 60" exec_delay_s=0 engine_bind_port=1001 | Out-Null
& "$PSScriptRoot\run_test.ps1" -Client -Wait $Wait -Tail 0 | Out-Null

function LogDir($install) { "$env:LOCALAPPDATA\MetroCoop\" + ((Split-Path $install -Leaf) -replace '[^A-Za-z0-9_-]', '_') }
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
$fail = 0
foreach ($k in $checks.Keys) { $ok = [bool]$checks[$k]; if (-not $ok) { $fail++ }; "{0}  {1}" -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $k }
"host  last: $(if ($hb.Count) { $hb[-1] -replace '.*(s0=.*?) net=.*', '$1' })"
"client last: $(if ($cb.Count) { $cb[-1] -replace '.*(s0=.*?) net=.*', '$1' })"
"RESULT: $(if ($fail) { "FAIL ($fail of $($checks.Count))" } else { "PASS ($($checks.Count) checks)" })"
exit $fail
