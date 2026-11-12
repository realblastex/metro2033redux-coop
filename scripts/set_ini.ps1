# Set keys in an instance's metro_coop.ini (creates it if missing).
# usage: set_ini.ps1 host|client key=value [key=value ...]
# Install folders: $env:METRO_HOST_DIR / $env:METRO_CLIENT_DIR (folders that contain metro.exe).
param([Parameter(Mandatory)][ValidateSet('host', 'client')][string]$Who,
      [Parameter(ValueFromRemainingArguments)][string[]]$KV)
$install = if ($Who -eq 'host') { $env:METRO_HOST_DIR } else { $env:METRO_CLIENT_DIR }
if (-not $install) { throw "set `$env:METRO_$($Who.ToUpper())_DIR to the folder that contains metro.exe" }
$dir = "$install\metro_coop"  # mod folder (instance_dir() in src/dllmain.cpp = folder of the DLL)
New-Item -ItemType Directory -Force $dir | Out-Null
$ini = "$dir\metro_coop.ini"
$lines = [System.Collections.ArrayList]@(if (Test-Path $ini) { Get-Content $ini })
foreach ($pair in $KV) {
    $k, $v = $pair -split '=', 2
    # drop every copy (the launcher appends its keys under [coop]; the DLL uses the last one), then add one
    $lines = [System.Collections.ArrayList]@($lines | Where-Object { $_ -notmatch "^\s*$k\s*=" })
    [void]$lines.Add("$k = $v")
}
Set-Content $ini $lines
"$Who ($ini):"; $lines
