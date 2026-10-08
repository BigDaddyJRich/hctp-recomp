# Saves a picture of the running game window (ps2EntryRunner) to -Out. PrintWindow, so the window may be covered.
# Used by run_test.ps1 (each capture in its own process with a timeout: a busy window can block PrintWindow).
param([string]$Out)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W2 {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public struct RECT { public int L, T, R, B; }
}
"@
[void][W2]::SetProcessDPIAware()
$p = Get-Process ps2EntryRunner | Select-Object -First 1
$h = $p.MainWindowHandle
$r = New-Object W2+RECT; [void][W2]::GetWindowRect($h, [ref]$r)
"rect $($r.L),$($r.T),$($r.R),$($r.B)"
$bmp = New-Object Drawing.Bitmap ([Math]::Max(1, $r.R - $r.L)), ([Math]::Max(1, $r.B - $r.T))
$g = [Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc(); [void][W2]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
$bmp.Save($Out); "saved $Out"
