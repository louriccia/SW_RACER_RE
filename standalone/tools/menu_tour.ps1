param([string]$Snap = "tour")
# Boot -> profile -> vehicle -> track -> info -> hangar main menu, then enter each main-menu item
# (Down n times + Enter), screenshot, leave with Escape. Reports any exception the loader logged.
$t = $PSScriptRoot
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
& "$t\to_hangar.ps1" -Snap "$Snap-veh" | Out-Null
& "$t\step.ps1" -Codes "1C" -Wait 5 -Snap "$Snap-track" | Out-Null
& "$t\step.ps1" -Codes "1C" -Wait 5 -Snap "$Snap-info" | Out-Null
& "$t\step.ps1" -Codes "1C" -Wait 8 -Snap "$Snap-main" | Out-Null
$items = @("inspect", "upgrades", "buyparts", "junkyard", "pitdroids", "changevehicle")
for ($i = 0; $i -lt $items.Count; $i++) {
    $down = @(); for ($k = 0; $k -le $i; $k++) { $down += "E050" }
    & "$t\step.ps1" -Codes (($down + "1C") -join ",") -Wait 6 -Snap "$Snap-$($items[$i])" | Out-Null
    $exc = (Get-Content "$g\standalone.log" | Select-String "EXCEPTION" | Select-Object -Last 1).Line
    $alive = [bool](Get-Process swe1r_standalone -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -ne "" })
    "$($items[$i]): alive=$alive $exc"
    if (-not $alive) { break }
    & "$t\step.ps1" -Codes "01" -Wait 5 -Snap "$Snap-$($items[$i])-back" | Out-Null
}
