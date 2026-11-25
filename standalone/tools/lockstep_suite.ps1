param([string[]]$Recordings = @("lockstep_n_t5_pod0.bin", "lockstep_n_t0_pod21.bin", "lockstep_n_t9_pod7.bin", "lockstep_n_t14_pod15_mir.bin", "lockstep_n_t20_pod3.bin"),
      [switch]$RefreshJunk)
# For each retail recording: get retail's frame-0 racer dump (junk_<rec>.bin, taken from a retail
# replay with `dump_at 0`, once), then replay the recording in the standalone and print the verdict.
$t = $PSScriptRoot
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
foreach ($rec in $Recordings) {
    $junk = "junk_" + [IO.Path]::GetFileNameWithoutExtension($rec) + ".bin"
    if ($RefreshJunk -or -not (Test-Path "$g\$junk")) {
        Remove-Item "$g\lockstep_dump_rep_0.bin" -ErrorAction SilentlyContinue
        & "$t\retail_lockstep_run.ps1" -Recording $rec -Extra "dump_at 0" -TimeoutSec 120 | Out-Null
        Stop-Process -Name SWEP1RCR -Force -ErrorAction SilentlyContinue
        if (Test-Path "$g\lockstep_dump_rep_0.bin") { Move-Item "$g\lockstep_dump_rep_0.bin" "$g\$junk" -Force } else { "$rec : no retail frame-0 dump"; continue }
    }
    $out = & "$t\lockstep_run.ps1" -Recording $rec -Extra "junk $junk" -TimeoutSec 150
    $v = ($out | Select-String "ended after|stopped at|TIMEOUT|scene mismatch|not found").Line -join " / "
    "$rec : $v"
}
