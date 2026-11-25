param([string]$Code = "48", [int]$Ms = 5000)
& "$PSScriptRoot\scan.ps1" -Codes "" -Gap 0 2>$null | Out-Null
$w = Get-Process $(if ($env:SA_PROC) { $env:SA_PROC } else { "swe1r_standalone" }) | Where-Object { $_.MainWindowTitle -ne "" } | Select-Object -First 1
[void][Scan]::SetForegroundWindow($w.MainWindowHandle)
Start-Sleep -Milliseconds 300
[Scan]::HoldX([Convert]::ToUInt16($Code, 16), $Ms, $true)
