param([string]$Snap = "race")
# Boot -> profile -> vehicle select -> track select -> track info -> hangar menu Start Race -> skip cutscene.
$t = $PSScriptRoot
& "$t\to_hangar.ps1" -Snap "$Snap-hangar" | Out-Null
& "$t\step.ps1" -Codes "1C" -Wait 5 -Snap "$Snap-1" | Out-Null
& "$t\step.ps1" -Codes "1C" -Wait 5 -Snap "$Snap-2" | Out-Null
& "$t\step.ps1" -Codes "1C" -Wait 12 -Snap "$Snap-3" | Out-Null
& "$t\step.ps1" -Codes "1C" -Wait 15 -Snap "$Snap-4" | Out-Null
& "$t\step.ps1" -Codes "1C" -Wait 15 -Snap $Snap
