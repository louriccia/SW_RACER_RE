param([string]$Recording = "lockstep_n_t5_pod0.bin", [int]$TimeoutSec = 300, [string]$Extra = "")
# Replays a lockstep recording in RETAIL (Steam SWEP1RCR.EXE + the deployed hook DLL in vanilla mode,
# no reimplementations live). reimpl_config.txt is moved aside for the run and restored after.
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
$canon = "C:\Users\louri\Documents\GitHub\SWR_bench\save_snapshots\lockstep_canonical"

Stop-Process -Name SWEP1RCR -Force -ErrorAction SilentlyContinue
Start-Sleep 1
Remove-Item -Recurse -Force "$g\data\player", "$g\data\config"
Copy-Item -Recurse "$canon\player" "$g\data\player"
Copy-Item -Recurse "$canon\config" "$g\data\config"
$hadCfg = Test-Path "$g\reimpl_config.txt"
if ($hadCfg) { Move-Item "$g\reimpl_config.txt" "$g\reimpl_config.txt.lockstep-aside" -Force }
$cfg = "replay $Recording`nautostart`nvanilla`nexit_when_done`n$($Extra -replace ';', "`n")`n"
[IO.File]::WriteAllText("$g\lockstep.txt", $cfg)
Remove-Item "$g\lockstep.log" -ErrorAction SilentlyContinue

$t0 = Get-Date
Start-Process "steam://rungameid/808910"
for ($i = 0; $i -lt 30 -and -not (Get-Process SWEP1RCR -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
# SteamStub relaunches the EXE, so wait on the harness log's verdict (or the process being gone for a while).
$gone = 0
while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
    Start-Sleep 2
    if ((Test-Path "$g\lockstep.log") -and (Select-String -Path "$g\lockstep.log" -Pattern "after \d+ frames" -Quiet)) { break }
    if (Get-Process SWEP1RCR -ErrorAction SilentlyContinue) { $gone = 0 } elseif (++$gone -ge 10) { "game exited without a verdict"; break }
}
Start-Sleep 2
if (Get-Process SWEP1RCR -ErrorAction SilentlyContinue) { Stop-Process -Name SWEP1RCR -Force; "TIMEOUT" }
"elapsed $([int]((Get-Date) - $t0).TotalSeconds) s"
[IO.File]::WriteAllText("$g\lockstep.txt", "# off`n")
if ($hadCfg) { Move-Item "$g\reimpl_config.txt.lockstep-aside" "$g\reimpl_config.txt" -Force }
if (Test-Path "$g\lockstep.log") { Get-Content "$g\lockstep.log" | Select-Object -Last 12 } else { "no lockstep.log" }
