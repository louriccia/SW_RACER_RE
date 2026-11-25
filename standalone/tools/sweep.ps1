param([int[]]$Tracks = (0..24), [int]$Frame = 1200, [string]$Retail = "rd*,std3D*,swrObjElmo*,swrModel*,swrViewport*,swrObjcMan*,swrRender*,swrScene*,Direct3d_*")
# A/B every track: pure standalone vs $Retail on retail bodies, frozen at $Frame; prints the pixel diff.
$t = $PSScriptRoot
$py = "C:/Users/louri/AppData/Local/Programs/Python/Python310/python.exe"
foreach ($n in $Tracks) {
    $a = & "$t\freeze_shot.ps1" -Track $n -Frame $Frame -Name "sw_a$n"
    $b = & "$t\freeze_shot.ps1" -Track $n -Frame $Frame -Name "sw_b$n" -Retail $Retail
    $d = & $py "$t\imgdiff.py" "$t\sw_a$n.png" "$t\sw_b$n.png" "$t\sw_d$n.png"
    "t$n : $d | $a | $b"
}
