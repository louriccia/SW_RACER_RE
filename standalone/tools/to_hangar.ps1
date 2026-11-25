param([string]$Snap = "h")
# Boot, skip movies, select the first profile and press OK, then screenshot.
$t = $PSScriptRoot
$W = "$env:windir\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
& "$t\run.ps1" -Boot 8 -Codes "1C,1C,1C,1C" -Gap 3 -Snap "$Snap-menu" | Out-Null
Start-Sleep 2
& $W -NoProfile -ExecutionPolicy Bypass -File "$t\scan.ps1" -Codes "E050" -Gap 2
& $W -NoProfile -ExecutionPolicy Bypass -File "$t\clickhere.ps1" -Clicks 1
Start-Sleep 2
& $W -NoProfile -ExecutionPolicy Bypass -File "$t\hold.ps1" -Code 1C -Ms 300
Start-Sleep 12
& "$t\snap.ps1" -Name $Snap | Select-Object -Last 1
Get-Content "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer\standalone.log" | Select-String "EXCEPTION|in C:|stack code" | Select-Object -Last 3
