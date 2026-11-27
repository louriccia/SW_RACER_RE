param([string]$Name, [int]$First, [int[]]$Then, [string]$Dll, [int]$Frames = 600, [int]$TimeoutSec = 900)
# Records a retail lockstep sequence ($Name.bin, $Name_r<k>.bin) with Steam SWEP1RCR.EXE and the hook DLL at
# -Dll (built from this tree), in vanilla mode, plus each race's frame-0 racer dump as junk_$Name[_r<k>].bin. Everything
# it touches in the game folder (dinput.dll, reimpl_config.txt, replacement sprites, saves, lockstep.txt, ini) is restored.
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
$canon = "C:\Users\louri\Documents\GitHub\SWR_bench\save_snapshots\lockstep_canonical"
$bak = Join-Path $env:TEMP "swr_retail_seq_bak"
Remove-Item -Recurse -Force $bak -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $bak | Out-Null
Copy-Item "$g\dinput.dll" "$bak\dinput.dll"
Copy-Item -Recurse "$g\data\player" "$bak\player"
Copy-Item -Recurse "$g\data\config" "$bak\config"
Copy-Item "$g\lockstep.txt" "$bak\lockstep.txt"
Copy-Item "$g\SW_RACER_RE.ini" "$bak\SW_RACER_RE.ini"
$hadCfg = Test-Path "$g\reimpl_config.txt"
if ($hadCfg) { Move-Item "$g\reimpl_config.txt" "$bak\reimpl_config.txt" -Force }
$hadSprites = Test-Path "$g\assets\replacement_sprites"
if ($hadSprites) { Rename-Item "$g\assets\replacement_sprites" "replacement_sprites.lockstep-aside" }
try {
    Stop-Process -Name SWEP1RCR -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
    Copy-Item $Dll "$g\dinput.dll" -Force
    Remove-Item -Recurse -Force "$g\data\player", "$g\data\config"
    Copy-Item -Recurse "$canon\player" "$g\data\player"
    Copy-Item -Recurse "$canon\config" "$g\data\config"
    Remove-Item "$g\lockstep.log", "$g\lockstep_dump_rec_0*.bin" -ErrorAction SilentlyContinue
    $thenLines = ($Then | ForEach-Object { "then $_" }) -join "`n"
    [IO.File]::WriteAllText("$g\lockstep.txt", "record $Name.bin`nautostart`nvanilla`nexit_when_done`ntrack $First`npod 0`nframes $Frames`ndump_at 0`n$thenLines`n")
    $t0 = Get-Date
    Start-Process "steam://rungameid/808910"
    $v = "TIMEOUT"
    while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
        Start-Sleep 3
        if (Test-Path "$g\lockstep.log") {
            $l = (Select-String -Path "$g\lockstep.log" -Pattern "sequence of|cannot|recorded \d+ frames" | Select-Object -Last 1)
            if ($l -and $l.Line -match "sequence of|cannot") { $v = $l.Line; break }
        }
        if (((Get-Date) - $t0).TotalSeconds -gt 30 -and -not (Get-Process SWEP1RCR -ErrorAction SilentlyContinue)) { $v = "EXITED"; break }
    }
    Start-Sleep 2
    Stop-Process -Name SWEP1RCR -Force -ErrorAction SilentlyContinue
    Start-Sleep 2
    "verdict: $v ($([int]((Get-Date) - $t0).TotalSeconds) s)"
    Get-Content "$g\lockstep.log" -ErrorAction SilentlyContinue | Select-String "race|autostart|cannot" | ForEach-Object { $_.Line }
    Get-ChildItem "$g\lockstep_dump_rec_0*.bin" -ErrorAction SilentlyContinue | ForEach-Object {
        $suffix = $_.BaseName.Substring("lockstep_dump_rec_0".Length)
        Move-Item $_.FullName "$g\junk_$Name$suffix.bin" -Force
    }
    Copy-Item "$g\lockstep.log" "$g\$Name.lockstep.log" -ErrorAction SilentlyContinue
} finally {
    Stop-Process -Name SWEP1RCR -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
    Copy-Item "$bak\dinput.dll" "$g\dinput.dll" -Force
    Remove-Item -Recurse -Force "$g\data\player", "$g\data\config"
    Copy-Item -Recurse "$bak\player" "$g\data\player"
    Copy-Item -Recurse "$bak\config" "$g\data\config"
    Copy-Item "$bak\lockstep.txt" "$g\lockstep.txt" -Force
    Copy-Item "$bak\SW_RACER_RE.ini" "$g\SW_RACER_RE.ini" -Force
    if ($hadCfg) { Move-Item "$bak\reimpl_config.txt" "$g\reimpl_config.txt" -Force }
    if ($hadSprites) { Rename-Item "$g\assets\replacement_sprites.lockstep-aside" "replacement_sprites" }
    "restored; dinput.dll md5 now $((Get-FileHash "$g\dinput.dll" -Algorithm MD5).Hash)"
}
