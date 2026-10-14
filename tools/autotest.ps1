# HCTP automated test: boot, clear ATTENTION / memory-card screens, sit idle on PRESS START until the attract-mode demo
# match runs; screenshot the game window every N seconds; dump the audio; stop after -Seconds.
# Output: F:\hctp-newbase\logs\auto\ (game.err, shot_NNN.png, audio.pcm).
# Usage: powershell -ExecutionPolicy Bypass -File F:\hctp-newbase\tools\autotest.ps1 [-Seconds 240] [-Every 5] [-Extra "VENDETTA_VU1_COMPILED=0"]
param(
    [int]$Seconds = 240,
    [int]$Every = 5,
    [string]$Pad = "20:START,30:START",
    [string[]]$Extra = @(),
    [string]$Exe = 'F:\hctp-newbase\PS2Recomp\out\fast\ps2xRuntime\ps2EntryRunner.exe'
)
$ErrorActionPreference = 'Stop'
$out = 'F:\hctp-newbase\logs\auto'
if (Test-Path $out) { Get-ChildItem $out -File | Remove-Item -Force }
New-Item -ItemType Directory -Force $out | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W32 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[W32]::SetProcessDPIAware() | Out-Null

$env:HCTP_MOVIES = '1'
$env:HCTP_PCPROF = '1'
$env:VENDETTA_HOSTPROF = '1'
$env:VENDETTA_SPULOG = '1'
$env:VENDETTA_AUDIOLOG = '1'
$env:VENDETTA_PADSCRIPT = $Pad
$env:VENDETTA_AUDIODUMP = "$out\audio.pcm"
foreach ($kv in ($Extra -join ",").Split(",", [StringSplitOptions]::RemoveEmptyEntries)) { $k, $v = $kv.Split('=', 2); Set-Item -Path "env:$k" -Value $v }

$exe = $Exe
$p = Start-Process -FilePath $exe -ArgumentList 'SLUS_207.87' -WorkingDirectory 'F:\HCTP MODS\Vanilla' `
    -RedirectStandardError "$out\game.err" -RedirectStandardOutput "$out\game.out" -PassThru

$start = Get-Date
$n = 0
while (((Get-Date) - $start).TotalSeconds -lt $Seconds -and -not $p.HasExited) {
    Start-Sleep -Seconds $Every
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { continue }
    $r = New-Object W32+RECT
    [W32]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = New-Object W32+POINT
    [W32]::ClientToScreen($h, [ref]$pt) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { continue }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
    $t = [int]((Get-Date) - $start).TotalSeconds
    $bmp.Save(("{0}\shot_{1:D3}_t{2:D3}.png" -f $out, $n, $t), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    $n++
}
if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null; Start-Sleep -Seconds 3 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
"done: $n shots in $out"
