param([string[]]$Aside)
# Retail lockstep replay with the named replacement sprites moved out of the folder; prints the
# verdict. Everything is moved back afterwards.
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
$rs = "$g\assets\replacement_sprites"
$tmp = "C:\Users\louri\AppData\Local\Temp\claude\1fe9acfddfa54f7a\56a4db6f-9dd5-4941-b134-b92355e0e84e\scratchpad\sprites_aside"
New-Item -ItemType Directory -Force $tmp | Out-Null
$moved = @()
foreach ($a in $Aside) { if (Test-Path "$rs\$a") { Move-Item "$rs\$a" $tmp -Force; $moved += $a } }
try {
    $r = & "$PSScriptRoot\retail_lockstep_run.ps1" -Recording lockstep_n_t5_pod0.bin -TimeoutSec 120
    $v = ($r | Select-String "after \d+ frames|without a verdict").Line -join " / "
    "aside [$($moved -join ',')]: $v"
} finally {
    Stop-Process -Name SWEP1RCR -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
    foreach ($a in $moved) { Move-Item "$tmp\$a" $rs -Force }
}
