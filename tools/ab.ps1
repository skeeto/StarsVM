# ab.ps1 - time two emulator builds against each other, fairly.
#
# Each run is a full tools/bench.ps1 run, so every timed run is also checked
# against the golden output: a build that is fast because it is wrong fails
# here rather than winning.  The time is the emulator's own "Stopped:" figure,
# first instruction to last, which leaves out process start and the copying
# bench.ps1 does.
#
# The two builds alternate A B B A, round after round, after one warm-up run
# of each that is not counted, so that a drift over the session - thermals,
# something starting in the background - lands on both equally.  Each run is
# pinned to one logical CPU at high priority from its first instruction (see
# bench.ps1's -Affinity); the default is logical CPU 4, a performance core on
# the i9-12900 this was written on, and not CPU 0, which takes interrupts.
#
# What it prints is each build's median, and the ratio B/A of the medians and
# of every round's pair.  A difference is only worth believing if every
# round's ratio falls on the same side of 1: run A against a copy of itself
# first to see what the noise looks like.
#
# Usage:
#   tools/ab.ps1 -A .\old.exe -B .\new.exe
#   tools/ab.ps1 -A .\old.exe -B .\new.exe -Turns 50 -Rounds 5 -Extra "--no-native"

param(
    [Parameter(Mandatory = $true)][string]$A,
    [Parameter(Mandatory = $true)][string]$B,
    [int]$Turns = 10,
    [int]$Rounds = 7,
    [string]$Extra = "",
    [string]$Affinity = "0x10"
)

$ErrorActionPreference = "Stop"
$bench = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "bench.ps1"

function Run-One([string]$exe) {
    $argv = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $bench,
              "-Turns", $Turns, "-Exe", $exe, "-Affinity", $Affinity)
    if ($Extra) { $argv += @("-Extra", $Extra) }
    $out = & powershell @argv 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $out -notmatch "identical to golden") {
        Write-Output $out
        throw "$exe did not reproduce the golden output"
    }
    if ($out -notmatch "Stopped: halted after \d+ instructions in ([0-9.]+) s") {
        Write-Output $out
        throw "$exe printed no time"
    }
    return [double]$Matches[1]
}

function Median([double[]]$v) {
    $s = $v | Sort-Object
    $n = $s.Count
    if ($n % 2) { return $s[($n - 1) / 2] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2
}

"A = $A"
"B = $B"
"{0} turns{1}, {2} rounds, affinity {3}" -f $Turns,
    $(if ($Extra) { " $Extra" } else { "" }), $Rounds, $Affinity

Run-One $A | Out-Null
Run-One $B | Out-Null

$ta = @(); $tb = @(); $ratios = @()
for ($i = 0; $i -lt $Rounds; $i++) {
    if ($i % 2 -eq 0) { $x = Run-One $A; $y = Run-One $B }
    else              { $y = Run-One $B; $x = Run-One $A }
    $ta += $x; $tb += $y; $ratios += $y / $x
    "round {0}:  A {1:N3} s  B {2:N3} s  B/A {3:N4}" -f ($i + 1), $x, $y, ($y / $x)
}

$ma = Median $ta; $mb = Median $tb
$above = @($ratios | Where-Object { $_ -gt 1 }).Count
$below = @($ratios | Where-Object { $_ -lt 1 }).Count
"median:   A {0:N3} s  B {1:N3} s  B/A {2:N4}  ({3:+0.0;-0.0}%)" -f $ma, $mb,
    ($mb / $ma), (($mb / $ma - 1) * 100)
"rounds:   {0} with B slower, {1} with B faster{2}" -f $above, $below,
    $(if ($above -eq $Rounds -or $below -eq $Rounds) { " - consistent" }
      else { " - within the noise" })
