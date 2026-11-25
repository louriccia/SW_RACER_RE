param([int]$Wait = 0, [string]$Name = "snap")
if ($Wait -gt 0) { Start-Sleep $Wait }
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
Get-Process swe1r_standalone -ErrorAction SilentlyContinue | Select-Object Id, MainWindowTitle, @{n='MB';e={[int]($_.WorkingSet64/1MB)}}, @{n='CPU';e={[int]$_.CPU}} | Format-Table | Out-String
Get-Content "$g\standalone.log" | Select-Object -Skip 21
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$gr = [System.Drawing.Graphics]::FromImage($bmp)
$gr.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$bmp.Save("$PSScriptRoot\$Name.png")
"saved $Name.png"
