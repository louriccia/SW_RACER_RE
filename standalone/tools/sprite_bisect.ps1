# Which replacement sprite file makes the retail race start crash? Bisects the folder's files
# (single culprit assumed); every probe restores the folder.
$t = $PSScriptRoot
$rs = "C:\Program Files (x86)\Steam\steamapps\common\Star Wars Episode I Racer\assets\replacement_sprites"
$files = [string[]](Get-ChildItem -File $rs | ForEach-Object { $_.Name })
while ($files.Length -gt 1) {
    $k = [int][Math]::Floor($files.Length / 2)
    $half = [string[]]$files[0..($k - 1)]
    $r = & "$t\sprite_probe.ps1" -Aside $half
    "[$($files.Length)] aside $($half.Length): $(if ($r -match 'ALL MATCH') {'OK'} else {'CRASH'})"
    if ($r -match "ALL MATCH") { $files = $half } else { $files = [string[]]$files[$k..($files.Length - 1)] }
}
"CULPRIT: $files"
