param([string]$Exe, [string]$Cfg, [string]$Mode = "on", [string]$Out = "", [int]$TimeoutSec = 400)
# One swe1r run (sequence replays too) with the given lockstep.txt lines (';'-separated), canonical profile, saves +
# lockstep.txt snapshotted and restored. Prints the lockstep verdict lines.
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
$canon = "C:\Users\louri\Documents\GitHub\SWR_bench\save_snapshots\lockstep_canonical"
$tools = $PSScriptRoot
$W = "$env:windir\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
$bak = Join-Path $env:TEMP "swr_seq_run_bak"
$env:SA_PROC = "swe1r"
if ($Mode -eq "off") { $env:SWR_REFINED_OFF = "1" } else { Remove-Item Env:SWR_REFINED_OFF -ErrorAction SilentlyContinue }
Remove-Item -Recurse -Force $bak -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $bak | Out-Null
Copy-Item -Recurse "$g\data\player" "$bak\player"
Copy-Item -Recurse "$g\data\config" "$bak\config"
Copy-Item "$g\lockstep.txt" "$bak\lockstep.txt"
try {
    Stop-Process -Name swe1r -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
    Remove-Item -Recurse -Force "$g\data\player", "$g\data\config"
    Copy-Item -Recurse "$canon\player" "$g\data\player"
    Copy-Item -Recurse "$canon\config" "$g\data\config"
    [IO.File]::WriteAllText("$g\lockstep.txt", (($Cfg -split ';') -join "`n") + "`n")
    Remove-Item "$g\lockstep.log", "$g\standalone.log" -ErrorAction SilentlyContinue
    $t0 = Get-Date
    $p = Start-Process -FilePath $Exe -WorkingDirectory $g -PassThru
    Start-Sleep 8
    for ($i = 0; $i -lt 4; $i++) {
        & $W -NoProfile -ExecutionPolicy Bypass -File "$tools\scan.ps1" -Codes "1C" -Gap 2 2>$null | Out-Null
    }
    while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) { Start-Sleep 2 }
    if (-not $p.HasExited) { Stop-Process -Name swe1r -Force -ErrorAction SilentlyContinue; "TIMEOUT" }
    "exit $($p.ExitCode), $([int]((Get-Date) - $t0).TotalSeconds) s"
    Get-Content "$g\lockstep.log" -ErrorAction SilentlyContinue | Select-String "race|ALL MATCH|mismatch|sequence|cannot|not a|seeded|then" | ForEach-Object { $_.Line }
    Get-Content "$g\standalone.log" | Select-String "EXCEPTION|invariant" | ForEach-Object { $_.Line }
    if ($Out) { Copy-Item "$g\standalone.log" "$Out.standalone.log"; Copy-Item "$g\lockstep.log" "$Out.lockstep.log" -ErrorAction SilentlyContinue }
} finally {
    Stop-Process -Name swe1r -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
    Remove-Item -Recurse -Force "$g\data\player", "$g\data\config"
    Copy-Item -Recurse "$bak\player" "$g\data\player"
    Copy-Item -Recurse "$bak\config" "$g\data\config"
    Copy-Item "$bak\lockstep.txt" "$g\lockstep.txt" -Force
    "restored"
}
