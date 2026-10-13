param([string]$Dir)
Add-Type -AssemblyName System.Drawing
$f = Get-ChildItem $Dir -Filter 'shot_*.png' | Sort-Object Name | Select-Object -Last 1
$b = New-Object System.Drawing.Bitmap $f.FullName
$sum = 0; $n = 0
for ($y = 0; $y -lt $b.Height; $y += 16) { for ($x = 0; $x -lt $b.Width; $x += 16) { $p = $b.GetPixel($x, $y); $sum += $p.R + $p.G + $p.B; $n++ } }
$b.Dispose()
[int]($sum / $n / 3)
