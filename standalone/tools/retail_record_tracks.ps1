param([int[]]$Tracks = (0..24), [int]$Frames = 1500, [int]$Pod = 0)
# Records a vanilla retail baseline per track (Steam SWEP1RCR.EXE + deployed hook DLL, no reimpls)
# with autostart, plus retail's frame-0 racer dump for `junk`. Files: sa_t<N>.bin, junk_sa_t<N>.bin.
# Puts aside reimpl_config.txt and the replacement sprites (they crash GL at race start until the
# UI-anchor PR lands), uses the canonical lockstep profile, and restores everything afterwards.
$g = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer"
$canon = "C:\Users\louri\Documents\GitHub\SWR_bench\save_snapshots\lockstep_canonical"
$hadCfg = Test-Path "$g\reimpl_config.txt"
if ($hadCfg) { Move-Item "$g\reimpl_config.txt" "$g\reimpl_config.txt.user" -Force }
Rename-Item "$g\assets\replacement_sprites" "replacement_sprites.lockstep-aside"
try {
    foreach ($n in $Tracks) {
        Stop-Process -Name SWEP1RCR -Force -ErrorAction SilentlyContinue
        Start-Sleep 1
        Remove-Item -Recurse -Force "$g\data\player", "$g\data\config"
        Copy-Item -Recurse "$canon\player" "$g\data\player"; Copy-Item -Recurse "$canon\config" "$g\data\config"
        Remove-Item "$g\lockstep.log", "$g\lockstep_dump_rec_0.bin" -ErrorAction SilentlyContinue
        [IO.File]::WriteAllText("$g\lockstep.txt", "record sa_t$n.bin`nautostart`nvanilla`nexit_when_done`ntrack $n`npod $Pod`nframes $Frames`ndump_at 0`n")
        $t0 = Get-Date
        Start-Process "steam://rungameid/808910"
        $v = "TIMEOUT"
        while (((Get-Date) - $t0).TotalSeconds -lt 150) {
            Start-Sleep 2
            if (Test-Path "$g\lockstep.log") {
                $l = (Select-String -Path "$g\lockstep.log" -Pattern "after \d+ frames|cannot|mismatch \(" | Select-Object -First 1)
                if ($l) { $v = $l.Line; break }
            }
            if (((Get-Date) - $t0).TotalSeconds -gt 20 -and -not (Get-Process SWEP1RCR -ErrorAction SilentlyContinue)) { $v = "EXITED"; break }
        }
        Start-Sleep 2
        Stop-Process -Name SWEP1RCR -Force -ErrorAction SilentlyContinue
        if (Test-Path "$g\lockstep_dump_rec_0.bin") { Move-Item "$g\lockstep_dump_rec_0.bin" "$g\junk_sa_t$n.bin" -Force }
        $auto = (Select-String -Path "$g\lockstep.log" -Pattern "autostart track" -ErrorAction SilentlyContinue | Select-Object -First 1).Line
        "t$n : $v | $auto"
    }
} finally {
    Stop-Process -Name SWEP1RCR -Force -ErrorAction SilentlyContinue
    Start-Sleep 1
    [IO.File]::WriteAllText("$g\lockstep.txt", "# off`n")
    if ($hadCfg) { Move-Item "$g\reimpl_config.txt.user" "$g\reimpl_config.txt" -Force }
    Rename-Item "$g\assets\replacement_sprites.lockstep-aside" "replacement_sprites"
}
