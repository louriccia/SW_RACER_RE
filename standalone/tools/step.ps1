param([string]$Codes = "1C", [int]$HoldMs = 250, [int]$Wait = 4, [string]$Snap = "step")
# Press one or more scan codes (comma list, "E0xx" = extended) on the running game, wait, screenshot.
$t = $PSScriptRoot
$W = "$env:windir\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
foreach ($c in $Codes.Split(",")) {
    if ($c.StartsWith("E0") -and $c.Length -gt 2) {
        & $W -NoProfile -ExecutionPolicy Bypass -File "$t\scan.ps1" -Codes $c -Gap 1
    } else {
        & $W -NoProfile -ExecutionPolicy Bypass -File "$t\hold.ps1" -Code $c -Ms $HoldMs
        Start-Sleep 1
    }
}
Start-Sleep $Wait
& "$t\snap.ps1" -Name $Snap | Select-Object -Last 1
Get-Content "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer\standalone.log" | Select-String "EXCEPTION|in C:|stack code" | Select-Object -Last 3
