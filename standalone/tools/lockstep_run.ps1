param([string]$Recording = "lockstep_n_t5_pod0.bin", [int]$TimeoutSec = 400, [string]$Extra = "")
# Replays a retail lockstep recording in the standalone EXE and prints the verdict from lockstep.log.
# Restores the canonical lockstep profile first (the recordings were made from it); the caller restores
# the player's own saves afterwards.
$t = $PSScriptRoot
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
$canon = "C:\Users\louri\Documents\GitHub\SWR_bench\save_snapshots\lockstep_canonical"
$W = "$env:windir\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"

Stop-Process -Name swe1r_standalone -Force -ErrorAction SilentlyContinue
Start-Sleep 1
Remove-Item -Recurse -Force "$g\data\player", "$g\data\config"
Copy-Item -Recurse "$canon\player" "$g\data\player"
Copy-Item -Recurse "$canon\config" "$g\data\config"
$cfg = "replay $Recording`nautostart`nvanilla`nexit_when_done`n$($Extra -replace ';', "`n")`n"
[IO.File]::WriteAllText("$g\lockstep.txt", $cfg)
Remove-Item "$g\lockstep.log", "$g\standalone.log" -ErrorAction SilentlyContinue

$t0 = Get-Date
$p = Start-Process -FilePath "C:\Users\louri\Documents\GitHub\SWR_wt_standalone\build_sa\swe1r_standalone.exe" -WorkingDirectory $g -PassThru
Start-Sleep 8
for ($i = 0; $i -lt 4; $i++) {
    & $W -NoProfile -ExecutionPolicy Bypass -File "$t\scan.ps1" -Codes "1C" -Gap 2 2>$null | Out-Null
}
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) { Start-Sleep 2 }
if (-not $p.HasExited) { Stop-Process -Name swe1r_standalone -Force -ErrorAction SilentlyContinue; "TIMEOUT after $TimeoutSec s" }
"elapsed $([int]((Get-Date) - $t0).TotalSeconds) s, exit $($p.ExitCode)"
[IO.File]::WriteAllText("$g\lockstep.txt", "# off`n")
if (Test-Path "$g\lockstep.log") { Get-Content "$g\lockstep.log" | Select-Object -Last 15 } else { "no lockstep.log" }
Get-Content "$g\standalone.log" | Select-String "lockstep:|EXCEPTION|in C:|stack code" | Select-Object -Last 6
