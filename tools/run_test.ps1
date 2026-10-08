# A test run without the user. Usage (from the repo root):
#   powershell -ExecutionPolicy Bypass -File tools\run_test.ps1 -Name runN -Seconds '60 120' -Stop 150
# Starts the game (logs to shots\<Name>.log / .err), saves a window picture at each of -Seconds (each capture in its own
# process, given up after 15 s so a blocked window cannot hang the run), stops the game at -Stop seconds.
# Times are ONE string: through powershell -File a comma list arrives as one string, and an en-GB machine reads
# '90,150' as 90150. Env switches pass through, e.g.
#   $env:VENDETTA_PADSCRIPT='50:START,55:START,60:START,65:START'   (reaches a CPU match ~60-90 s in)
#   $env:VENDETTA_HOSTPROF='1' (or '2' = + code-address profile), VENDETTA_VU1_VERIFY=1, VENDETTA_IOP_VERIFY=1,
#   VENDETTA_THREADS=1, VENDETTA_AUDIOLOG=1.
# -Close: at -Stop, close the window like the user would (not kill the process) and report how long the exit took.
param([string]$Name, [string]$Seconds = '', [int]$Stop = 120, [switch]$Close)
$times = @($Seconds -split '[ ;,]+' | Where-Object { $_ -ne '' } | ForEach-Object { [int]$_ })
$r = Split-Path -Parent $PSScriptRoot
$p = Start-Process -FilePath "$r\PS2Recomp\out\fast\ps2xRuntime\ps2EntryRunner.exe" -ArgumentList "`"$r\game\SLUS_206.39`"" -WorkingDirectory $r -RedirectStandardOutput "$r\shots\$Name.log" -RedirectStandardError "$r\shots\$Name.log.err" -PassThru
"started pid $($p.Id)"
$start = Get-Date
foreach ($s in $times) {
  while (((Get-Date) - $start).TotalSeconds -lt $s) { Start-Sleep -Milliseconds 250; if ($p.HasExited) { break } }
  if ($p.HasExited) { break }
  $c = Start-Process powershell -ArgumentList "-ExecutionPolicy Bypass -File `"$PSScriptRoot\cap_window.ps1`" -Out `"$r\shots\$Name-$($s)s.png`"" -WindowStyle Hidden -PassThru
  if (-not $c.WaitForExit(15000)) { Stop-Process -Id $c.Id -Confirm:$false; "capture at $s s gave up" } else { "captured $s s" }
}
while (((Get-Date) - $start).TotalSeconds -lt $Stop -and -not $p.HasExited) { Start-Sleep -Milliseconds 250 }
if ($p.HasExited) { "exited early at $([int]((Get-Date) - $start).TotalSeconds) s, code $($p.ExitCode)" }
elseif ($Close) {
  $closing = Get-Date
  [void]$p.CloseMainWindow()
  if ($p.WaitForExit(10000)) { "closed at $Stop s: exited in $([int]((Get-Date) - $closing).TotalMilliseconds) ms, code $($p.ExitCode)" }
  else { Stop-Process -Id $p.Id -Confirm:$false; "closed at $Stop s: still running after 10 s, killed" }
}
else { Stop-Process -Id $p.Id -Confirm:$false; "stopped at $Stop s" }
