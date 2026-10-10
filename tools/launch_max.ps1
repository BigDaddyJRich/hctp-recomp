# Starts the recompiled game for the user with their settings.toml (maximised by default, their preference);
# stdout / stderr -> shots\user.log / .err.
# Env switches pass through (e.g. $env:VENDETTA_HOSTPROF='1' to read their match's frame rate from the log afterwards).
# Usage (from the repo root): powershell -ExecutionPolicy Bypass -File tools\launch_max.ps1
$r = Split-Path -Parent $PSScriptRoot
$p = Start-Process -FilePath "$r\PS2Recomp\out\fast\ps2xRuntime\ps2EntryRunner.exe" -ArgumentList "`"$r\game\SLUS_206.39`"" -WorkingDirectory $r -RedirectStandardOutput "$r\shots\user.log" -RedirectStandardError "$r\shots\user.log.err" -PassThru
for ($i = 0; $i -lt 100; $i++) { Start-Sleep -Milliseconds 100; $p.Refresh(); if ($p.MainWindowHandle -ne 0 -and $p.MainWindowTitle -match '^(PS2-Recomp|Def Jam Vendetta)') { break } }
"started pid $($p.Id)"
