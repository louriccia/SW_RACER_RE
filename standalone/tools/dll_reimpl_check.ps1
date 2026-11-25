param([string]$Dll, [string]$Reimpl, [string]$Recording = "lockstep_n_t5_pod0.bin")
# Retail + the given hook DLL with only $Reimpl live (reimpl_config.txt), replaying a vanilla recording.
# Deployed dinput.dll, reimpl_config.txt and the replacement sprites (they crash the GL renderer at
# race start today) are put back afterwards.
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
$sp = "C:\Users\louri\AppData\Local\Temp\claude\1fe9acfddfa54f7a\56a4db6f-9dd5-4941-b134-b92355e0e84e\scratchpad"
if (-not (Test-Path "$sp\dinput_deployed_backup.dll")) { Copy-Item "$g\dinput.dll" "$sp\dinput_deployed_backup.dll" }
Copy-Item $Dll "$g\dinput.dll" -Force
Rename-Item "$g\assets\replacement_sprites" "replacement_sprites.lockstep-aside"
$hadCfg = Test-Path "$g\reimpl_config.txt"
if ($hadCfg) { Move-Item "$g\reimpl_config.txt" "$g\reimpl_config.txt.user" -Force }
try {
    [IO.File]::WriteAllText("$g\reimpl_config.txt", "reimpl $Reimpl`n")
    # retail_lockstep_run.ps1 moves reimpl_config aside itself; give it ours via its Extra-less path
    $canon = "C:\Users\louri\Documents\GitHub\SWR_bench\save_snapshots\lockstep_canonical"
    Remove-Item -Recurse -Force "$g\data\player", "$g\data\config"
    Copy-Item -Recurse "$canon\player" "$g\data\player"; Copy-Item -Recurse "$canon\config" "$g\data\config"
    [IO.File]::WriteAllText("$g\lockstep.txt", "replay $Recording`nautostart`nvanilla`nexit_when_done`nstop_at_mismatch`n")
    Remove-Item "$g\lockstep.log" -ErrorAction SilentlyContinue
    $t0 = Get-Date
    Start-Process "steam://rungameid/808910"
    while (((Get-Date) - $t0).TotalSeconds -lt 200) {
        Start-Sleep 2
        if ((Test-Path "$g\lockstep.log") -and (Select-String -Path "$g\lockstep.log" -Pattern "after \d+ frames" -Quiet)) { break }
    }
    Start-Sleep 2
    (Get-Content "$g\lockstep.log" | Select-String "after \d+ frames").Line
    (Get-Content "$g\hook.log" | Select-String "Hooked \[|live").Line | Select-Object -First 2
} finally {
    Stop-Process -Name SWEP1RCR -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
    [IO.File]::WriteAllText("$g\lockstep.txt", "# off`n")
    Remove-Item "$g\reimpl_config.txt" -Force
    if ($hadCfg) { Move-Item "$g\reimpl_config.txt.user" "$g\reimpl_config.txt" -Force }
    Rename-Item "$g\assets\replacement_sprites.lockstep-aside" "replacement_sprites"
    Copy-Item "$sp\dinput_deployed_backup.dll" "$g\dinput.dll" -Force
}
