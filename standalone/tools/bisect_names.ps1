param([string]$Base, [string]$Candidates, [string]$Recording = "lockstep_n_t5_pod0.bin")
# Bisects $Candidates (one function name per line): which name must run on retail for the replay to
# match? $Base is a list of rules that always run on retail. Assumes a single culprit.
$t = $PSScriptRoot
$sp = Split-Path $Candidates
$names = [string[]]([IO.File]::ReadAllLines($Candidates) | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
$base = [string[]]([IO.File]::ReadAllLines($Base) | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
while ($names.Length -gt 1) {
    $k = [int][Math]::Floor($names.Length / 2)
    $half = [string[]]$names[0..($k - 1)]
    $rest = [string[]]$names[$k..($names.Length - 1)]
    $probe = New-Object System.Collections.Generic.List[string]
    $probe.AddRange($base)
    $probe.AddRange($half)
    [IO.File]::WriteAllLines("$sp\bisect_probe.txt", $probe.ToArray())
    $r = & "$t\bisect_run.ps1" -List "$sp\bisect_probe.txt" -Recording $Recording -Label "[$($names.Length) -> retail $($half.Length)]"
    $r
    if ($r -match "ALL MATCH") { $names = $half } elseif ($r -match "first at frame") { $names = $rest } else { "inconclusive, stopping"; break }
}
"CULPRIT CANDIDATES: $($names -join ', ')"
