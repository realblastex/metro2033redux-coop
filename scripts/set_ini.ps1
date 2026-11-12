# Set keys in an instance's metro_coop.ini (creates it if missing).
# usage: set_ini.ps1 host|client key=value [key=value ...]
# Install folders: $env:METRO_HOST_DIR / $env:METRO_CLIENT_DIR (folders that contain metro.exe).
param([Parameter(Mandatory)][ValidateSet('host', 'client')][string]$Who,
      [Parameter(ValueFromRemainingArguments)][string[]]$KV)
$install = if ($Who -eq 'host') { $env:METRO_HOST_DIR } else { $env:METRO_CLIENT_DIR }
if (-not $install) { throw "set `$env:METRO_$($Who.ToUpper())_DIR to the folder that contains metro.exe" }
# same folder name rule as instance_dir() in src/dllmain.cpp
$name = (Split-Path $install -Leaf) -replace '[^A-Za-z0-9_-]', '_'
$dir = "$env:LOCALAPPDATA\MetroCoop\$name"
New-Item -ItemType Directory -Force $dir | Out-Null
$ini = "$dir\metro_coop.ini"
$lines = [System.Collections.ArrayList]@(if (Test-Path $ini) { Get-Content $ini })
foreach ($pair in $KV) {
    $k, $v = $pair -split '=', 2
    $i = 0..($lines.Count - 1) | Where-Object { $lines[$_] -match "^\s*$k\s*=" } | Select-Object -First 1
    if ($null -ne $i -and $lines.Count) { $lines[$i] = "$k = $v" } else { [void]$lines.Add("$k = $v") }
}
Set-Content $ini $lines
"$Who ($ini):"; $lines
