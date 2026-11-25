param([string]$Retail, [int]$Track = 1, [int]$Frame = 1200, [string]$Ref = "sw_b1", [string]$Name = "pr")
# One A/B bisect probe: frozen shot with $Retail on retail bodies, pixel diff against reference $Ref.
$t = $PSScriptRoot
$r = & "$t\freeze_shot.ps1" -Track $Track -Frame $Frame -Name $Name -Retail $Retail
$d = & "C:/Users/louri/AppData/Local/Programs/Python/Python310/python.exe" "$t\imgdiff.py" "$t\$Ref.png" "$t\$Name.png"
"[$Retail] vs ${Ref}: $d | $r"
