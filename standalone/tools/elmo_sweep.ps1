param([int[]]$Tracks = (0..24), [int]$Frame = 1400, [switch]$Autopilot,
      [string]$Retail = "swrObjElmo*,swrModel*,swrRace*,swrMath*,rdMatrix*,rdVector*,rdMath*,swrSpline*,stdMath*,swrObj_*,swrEvent*,rdPuppet*,swrObjHang*,swrObjJdge*,swrObjTest*")
# Puppet A/B per track: pure standalone vs $Retail on retail bodies, frozen at $Frame; diffs the
# elmo_dump lines (every puppet's state + root transform).
$t = $PSScriptRoot
foreach ($n in $Tracks) {
    $sfx = if ($Autopilot) { "ap" } else { "" }
    $a = & "$t\freeze_shot.ps1" -Track $n -Frame $Frame -Name "el${sfx}_a$n" -Autopilot:$Autopilot
    $b = & "$t\freeze_shot.ps1" -Track $n -Frame $Frame -Name "el${sfx}_b$n" -Retail $Retail -Autopilot:$Autopilot
    $la = @(Get-Content "$t\el${sfx}_a${n}_elmo.txt" -ErrorAction SilentlyContinue)
    $lb = @(Get-Content "$t\el${sfx}_b${n}_elmo.txt" -ErrorAction SilentlyContinue)
    $diff = 0; for ($i = 0; $i -lt [Math]::Max($la.Count, $lb.Count); $i++) { if ($la[$i] -ne $lb[$i]) { $diff++ } }
    "t$n : $($la.Count)/$($lb.Count) puppets, $diff differ | $a | $b"
}
