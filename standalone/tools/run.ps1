param([int]$Boot = 8, [string]$Codes = "1C", [int]$Gap = 3, [string]$Snap = "r")
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
if ($Boot -gt 0) {
    Stop-Process -Name swe1r_standalone -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
    Remove-Item "$g\standalone*.log" -ErrorAction SilentlyContinue
    $env:SWR_STANDALONE_WATCH = "1"
    if ($env:SA_RES) { $env:SWR_STANDALONE_RES = $env:SA_RES }
    Start-Process -FilePath "C:\Users\louri\Documents\GitHub\SWR_wt_standalone\build_sa\swe1r_standalone.exe" -WorkingDirectory $g | Out-Null
    Start-Sleep $Boot
}
& "$env:windir\SysWOW64\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\scan.ps1" -Codes $Codes -Gap $Gap -Snap $Snap
Get-Content "$g\standalone.log" | Select-String "EXCEPTION|in C:|stack code|reached RETAIL" | Select-Object -Last 6
