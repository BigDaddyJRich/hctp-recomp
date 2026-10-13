param([string]$Dir)
Add-Type -AssemblyName System.Drawing
$fs = Get-ChildItem $Dir -Filter 'shot_*.png' | Sort-Object Name
$W=320;$H=171;$cols=6;$rows=[math]::Ceiling($fs.Count/$cols)
$s = New-Object System.Drawing.Bitmap ($W*$cols),($H*$rows)
$g=[System.Drawing.Graphics]::FromImage($s)
$i=0; foreach($f in $fs){ $im=[System.Drawing.Image]::FromFile($f.FullName); $g.DrawImage($im,($i%$cols)*$W,[math]::Floor($i/$cols)*$H,$W,$H); $im.Dispose(); $i++ }
$s.Save("$Dir\sheet.jpg",[System.Drawing.Imaging.ImageFormat]::Jpeg)
