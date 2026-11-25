param([string[]]$Items = @("inspect", "upgrades", "buyparts", "junkyard", "pitdroids", "changevehicle"), [string]$Snap = "t2")
# From the hangar main menu with the first item in $Items highlighted: Enter, screenshot, Escape,
# then Down to the next item. The hangar menus read the arrows as a held axis, so keys are held.
$t = $PSScriptRoot
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
$W = "$env:windir\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
function Hold([string]$code, [int]$ms, [bool]$ext) {
    if ($ext) { & $W -NoProfile -ExecutionPolicy Bypass -File "$t\holdx.ps1" -Code $code -Ms $ms }
    else { & $W -NoProfile -ExecutionPolicy Bypass -File "$t\hold.ps1" -Code $code -Ms $ms }
}
foreach ($item in $Items) {
    Hold "1C" 300 $false; Start-Sleep 6
    & "$t\snap.ps1" -Name "$Snap-$item" | Out-Null
    $exc = (Get-Content "$g\standalone.log" | Select-String "EXCEPTION" | Select-Object -Last 1).Line
    $alive = [bool](Get-Process swe1r_standalone -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -ne "" })
    "${item}: alive=$alive $exc"
    if (-not $alive) { break }
    Hold "01" 300 $false; Start-Sleep 5
    Hold "50" 400 $true; Start-Sleep 2
}
