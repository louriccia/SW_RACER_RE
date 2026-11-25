param([string]$Code = "1C", [int]$Ms = 5000)
& "$PSScriptRoot\scan.ps1" -Codes "" -Gap 0 2>$null | Out-Null
$w = Get-Process swe1r_standalone | Where-Object { $_.MainWindowTitle -ne "" } | Select-Object -First 1
[void][Scan]::SetForegroundWindow($w.MainWindowHandle)
Start-Sleep -Milliseconds 300
[Scan]::Hold([Convert]::ToUInt16($Code, 16), $Ms)
