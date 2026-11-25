param([string]$List, [string]$Recording = "lockstep_n_t5_pod0.bin", [string]$Label = "")
# One bisect probe: run the standalone lockstep replay with the functions in $List on retail's
# bodies (decrypted image from SWR_bench) and print the first mismatching frame.
$env:SWR_STANDALONE_RETAIL_TEXT = "C:\Users\louri\Documents\GitHub\SWR_bench\retail_0x400000.bin"
$env:SWR_STANDALONE_RETAIL = $List
$out = & "$PSScriptRoot\lockstep_run.ps1" -Recording $Recording -Extra "junk rt_dump_0.bin;stop_at_mismatch" -TimeoutSec 150
Remove-Item Env:SWR_STANDALONE_RETAIL, Env:SWR_STANDALONE_RETAIL_TEXT
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
$sa = Get-Content "$g\standalone.log"
$retail = ($sa | Select-String "running retail: (\d+)").Matches | ForEach-Object { $_.Groups[1].Value }
$verdict = ($out | Select-String "first at frame (\d+)|ALL MATCH|TIMEOUT|seeded").Line -join " / "
$crash = ($sa | Select-String "EXCEPTION|in C:" | Select-Object -First 2).Line -join " / "
"$Label retail=$retail :: $verdict $crash"
