param([int]$Track = 0, [int]$Laps = 1, [int]$TimeoutSec = 420, [string]$Snap = "fin")
# Unattended full race in the standalone: autostart a $Laps-lap race on $Track with the local pod on
# autopilot (SWR_STANDALONE_AUTOPILOT), screenshot every 15 s, and leave the game running afterwards
# so the results screen / hangar return can be driven. Lockstep records (no compare) so autostart works.
# Saves the player's data/player + data/config first and uses the canonical lockstep profile.
$t = $PSScriptRoot
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
$canon = "C:\Users\louri\Documents\GitHub\SWR_bench\save_snapshots\lockstep_canonical"
$W = "$env:windir\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
$bak = "$env:TEMP\swr_finish_run_saves"

Stop-Process -Name swe1r_standalone -Force -ErrorAction SilentlyContinue
Start-Sleep 1
if (-not (Test-Path $bak)) {
    New-Item -ItemType Directory -Force $bak | Out-Null
    Copy-Item -Recurse "$g\data\player" "$bak\player"; Copy-Item -Recurse "$g\data\config" "$bak\config"
}
Remove-Item -Recurse -Force "$g\data\player", "$g\data\config"
Copy-Item -Recurse "$canon\player" "$g\data\player"; Copy-Item -Recurse "$canon\config" "$g\data\config"
[IO.File]::WriteAllText("$g\lockstep.txt", "record sa_finish.bin`nautostart`nvanilla`ntrack $Track`nlaps $Laps`npod 0`n")
Remove-Item "$g\lockstep.log", "$g\standalone.log" -ErrorAction SilentlyContinue

$env:SWR_STANDALONE_AUTOPILOT = "1"
$env:SWR_STANDALONE_WATCH = "1"
$t0 = Get-Date
$p = Start-Process -FilePath "C:\Users\louri\Documents\GitHub\SWR_wt_standalone\build_sa\swe1r_standalone.exe" -WorkingDirectory $g -PassThru
Start-Sleep 8
for ($i = 0; $i -lt 4; $i++) {
    & $W -NoProfile -ExecutionPolicy Bypass -File "$t\scan.ps1" -Codes "1C" -Gap 2 2>$null | Out-Null
}
# Judge state 4 (fly-by) waits for thrust or accept.
& $W -NoProfile -ExecutionPolicy Bypass -File "$t\hold.ps1" -Code "11" -Ms 600 2>$null | Out-Null
$n = 0
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
    Start-Sleep 15; $n++
    & "$t\snap.ps1" -Name "$Snap-$n" | Out-Null
    $l = if (Test-Path "$g\lockstep.log") { (Get-Content "$g\lockstep.log" | Select-Object -Last 1) } else { "" }
    "{0,4}s snap $Snap-$n | $l" -f [int]((Get-Date) - $t0).TotalSeconds
}
"exited=$($p.HasExited)"
Get-Content "$g\standalone.log" | Select-String "autopilot|lockstep:|EXCEPTION" | Select-Object -Last 6
