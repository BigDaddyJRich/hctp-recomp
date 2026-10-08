# Starts the recompiled game, waits, saves a picture of its window only (PrintWindow), then stops it.
# usage: snap_window.ps1 <exe> <elf> <out.png> [seconds...]  (one picture per listed time)
param([string]$Exe, [string]$Elf, [string]$Out, [int[]]$Seconds = @(30))
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public struct RECT { public int L, T, R, B; }
}
"@
[void][W]::SetProcessDPIAware()  # real pixel sizes, so display scaling doesn't crop the picture
$log = [IO.Path]::ChangeExtension($Out, '.log')
$p = Start-Process -FilePath $Exe -ArgumentList "`"$Elf`"" -WorkingDirectory (Split-Path $Elf -Parent | Split-Path -Parent) -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru
$start = Get-Date
foreach ($s in $Seconds) {
  while (((Get-Date) - $start).TotalSeconds -lt $s) { Start-Sleep -Milliseconds 250 }
  $p.Refresh()
  if ($p.HasExited) { "exited early, code $($p.ExitCode)"; break }
  $h = $p.MainWindowHandle
  $r = New-Object W+RECT; [void][W]::GetWindowRect($h, [ref]$r)
  $bmp = New-Object Drawing.Bitmap ([Math]::Max(1, $r.R - $r.L)), ([Math]::Max(1, $r.B - $r.T))
  $g = [Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc(); [void][W]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
  $file = $Out -replace '\.png$', "-$($s)s.png"; $bmp.Save($file); $g.Dispose(); $bmp.Dispose()
  "saved $file"
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id }
