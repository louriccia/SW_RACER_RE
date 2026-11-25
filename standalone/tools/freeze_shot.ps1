param([int]$Track = 0, [int]$Frame = 900, [string]$Retail = "", [string]$Name = "fz", [switch]$Autopilot)
# Replays the retail recording sa_t<Track>.bin in the standalone, freezes the image at race frame
# $Frame (SWR_STANDALONE_FREEZE_FRAME) and screenshots it to <Name>.png. $Retail = comma-separated
# names / prefix* to run on retail's decrypted bodies, for A/B against our src.
# The caller saves/restores data/player + data/config.
$t = $PSScriptRoot
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
$canon = "C:\Users\louri\Documents\GitHub\SWR_bench\save_snapshots\lockstep_canonical"
$W = "$env:windir\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
Stop-Process -Name swe1r_standalone -Force -ErrorAction SilentlyContinue
Start-Sleep 1
Remove-Item -Recurse -Force "$g\data\player", "$g\data\config"
Copy-Item -Recurse "$canon\player" "$g\data\player"; Copy-Item -Recurse "$canon\config" "$g\data\config"
$rec = "sa_t$Track.bin"; $junk = "`njunk junk_sa_t$Track.bin"
if ($Autopilot) {
    # The local pod drives itself. Inputs (the start press) are recorded once per track and frame
    # count, then replayed, so every run sees the same frames.
    $rec = "sa_ap_t${Track}_f$Frame.bin"; $junk = ""
    $env:SWR_STANDALONE_AUTOPILOT = "1"
    if (-not (Test-Path "$g\$rec")) {
        [IO.File]::WriteAllText("$g\lockstep.txt", "record $rec`nautostart`nvanilla`ntrack $Track`nlaps 1`npod 0`nframes $($Frame + 30)`nexit_when_done`n")
        $p = Start-Process -FilePath "C:\Users\louri\Documents\GitHub\SWR_wt_standalone\build_sa\swe1r_standalone.exe" -WorkingDirectory $g -PassThru
        Start-Sleep 8
        for ($i = 0; $i -lt 4; $i++) { & $W -NoProfile -ExecutionPolicy Bypass -File "$t\scan.ps1" -Codes "1C" -Gap 2 2>$null | Out-Null }
        & $W -NoProfile -ExecutionPolicy Bypass -File "$t\hold.ps1" -Code "11" -Ms 600 2>$null | Out-Null
        $p.WaitForExit(400000) | Out-Null
        Stop-Process -Name swe1r_standalone -Force -ErrorAction SilentlyContinue
        Start-Sleep 1
        Remove-Item -Recurse -Force "$g\data\player", "$g\data\config"
        Copy-Item -Recurse "$canon\player" "$g\data\player"; Copy-Item -Recurse "$canon\config" "$g\data\config"
    }
}
[IO.File]::WriteAllText("$g\lockstep.txt", "replay $rec`nautostart`nvanilla$junk`n")
Remove-Item "$g\lockstep.log", "$g\standalone.log", "$g\freeze.txt" -ErrorAction SilentlyContinue
$env:SWR_STANDALONE_FREEZE_FRAME = "$Frame"
if ($Retail) {
    $list = "$env:TEMP\swr_retail_list.txt"
    [IO.File]::WriteAllText($list, ($Retail -split ',') -join "`n")
    $env:SWR_STANDALONE_RETAIL_TEXT = "C:\Users\louri\Documents\GitHub\SWR_bench\retail_0x400000.bin"
    $env:SWR_STANDALONE_RETAIL = $list
}
$t0 = Get-Date
$p = Start-Process -FilePath "C:\Users\louri\Documents\GitHub\SWR_wt_standalone\build_sa\swe1r_standalone.exe" -WorkingDirectory $g -PassThru
Start-Sleep 8
for ($i = 0; $i -lt 4; $i++) { & $W -NoProfile -ExecutionPolicy Bypass -File "$t\scan.ps1" -Codes "1C" -Gap 2 2>$null | Out-Null }
while (-not (Test-Path "$g\freeze.txt") -and -not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 400) { Start-Sleep 1 }
Start-Sleep 2
& "$t\snap.ps1" -Name $Name | Out-Null
Stop-Process -Name swe1r_standalone -Force -ErrorAction SilentlyContinue
Remove-Item Env:SWR_STANDALONE_AUTOPILOT, Env:SWR_STANDALONE_FREEZE_FRAME, Env:SWR_STANDALONE_RETAIL, Env:SWR_STANDALONE_RETAIL_TEXT -ErrorAction SilentlyContinue
[IO.File]::WriteAllText("$g\lockstep.txt", "# off`n")
$retailN = (Get-Content "$g\standalone.log" | Select-String "running retail: (\d+)").Matches | ForEach-Object { $_.Groups[1].Value }
$exc = (Get-Content "$g\standalone.log" | Select-String "EXCEPTION" | Select-Object -First 1).Line
"$Name t$Track f$Frame frozen=$(Test-Path "$g\freeze.txt") retail=$retailN $exc"
if (Test-Path "$g\elmo_dump.txt") { Move-Item "$g\elmo_dump.txt" "$t\${Name}_elmo.txt" -Force }
