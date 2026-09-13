# bench.ps1 - time turn generation, and check that the turns came out right.
#
# The benchmark is the game's own batch mode: -gN on a host file generates N
# turns and exits.  With --fixed-clock every clock the guest can read is pinned,
# so two runs from the same files write byte-identical turn files, which is what
# makes a run comparable - a change to the interpreter that alters a single
# byte of output is a bug, whatever it did for speed.
#
# Layout, all under bench/ (which is ignored by git: the files are a saved game,
# which is the user's, and stars.exe, which is not ours to redistribute):
#
#   bench/orig/    Game.* plus stars.exe and Stars.ini, copied there by hand
#                  from wherever the test game lives.  Stars.ini sits beside
#                  the emulator, which is where it reads it from, and stars.exe
#                  beside that so no path has to be given; the game files
#                  could be anywhere the run is started from.
#   bench/run/     rebuilt from orig/ for every run
#   bench/goldenN/ the output of an N-turn run everybody agrees is right;
#                  -Golden (re)creates it from the run that follows
#
# Usage:
#   tools/bench.ps1                          ten turns with .\StarsVM.exe
#   tools/bench.ps1 -Turns 50                the late-game profile
#   tools/bench.ps1 -Exe .\StarsVM-prof.exe  the profiling build; its report
#                                            lands in bench/run.log
#   tools/bench.ps1 -Extra "--no-native"     extra emulator switches
#   tools/bench.ps1 -Golden -Extra "--no-native"
#                                            bless this run's output

param(
    [int]$Turns = 10,
    [string]$Exe = ".\StarsVM.exe",
    [string]$Extra = "",
    [switch]$Golden
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$bench = Join-Path $root "bench"
$orig = Join-Path $bench "orig"
$run = Join-Path $bench "run"
$gold = Join-Path $bench "golden$Turns"
$log = Join-Path $bench "run.log"

if (-not (Test-Path (Join-Path $orig "Game.hst"))) {
    Write-Error "no bench/orig/Game.hst: copy a test game there (Game.*, stars.exe, Stars.ini)"
}
$exePath = Resolve-Path (Join-Path $root $Exe) -ErrorAction SilentlyContinue
if (-not $exePath) { $exePath = Resolve-Path $Exe }

# A fresh run directory: the game files, the module, the ini, and the emulator
# under test, so that nothing from the previous run and nothing from the tree
# is in play.
if (Test-Path $run) { Remove-Item -Recurse -Force $run }
New-Item -ItemType Directory -Path $run | Out-Null
Copy-Item (Join-Path $orig "*") $run
$exeName = Split-Path -Leaf $exePath
Copy-Item $exePath (Join-Path $run $exeName)
if (Test-Path $log) { Remove-Item -Force $log }

$argv = @("--log", $log, "--fixed-clock")
if ($Extra) { $argv += ($Extra -split " ") }
$argv += @("--", "-g$Turns", "Game.hst")

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath (Join-Path $run $exeName) -ArgumentList $argv `
        -WorkingDirectory $run -Wait -PassThru -NoNewWindow
$sw.Stop()

$stopped = Select-String -Path $log -Pattern "^Stopped:" | Select-Object -Last 1
$native = Select-String -Path $log -Pattern "^  native:" | Select-Object -Last 1
"{0}  -g{1}  wall {2:N3} s  exit {3}" -f $exeName, $Turns, $sw.Elapsed.TotalSeconds, $p.ExitCode
if ($stopped) { $stopped.Line }
if ($native) { $native.Line }

# The outputs.  Game.* is everything the game writes beside the host file; the
# backup/ directory it also makes is copies of the same and is not compared.
$outputs = Get-ChildItem $run -Filter "Game.*" | Sort-Object Name
if ($Golden) {
    if (Test-Path $gold) { Remove-Item -Recurse -Force $gold }
    New-Item -ItemType Directory -Path $gold | Out-Null
    $outputs | ForEach-Object { Copy-Item $_.FullName $gold }
    "golden: {0} files blessed from this run" -f $outputs.Count
} elseif (Test-Path $gold) {
    $bad = 0
    foreach ($f in $outputs) {
        $g = Join-Path $gold $f.Name
        if (-not (Test-Path $g)) { "  MISSING in golden: $($f.Name)"; $bad++; continue }
        $a = [System.IO.File]::ReadAllBytes($f.FullName)
        $b = [System.IO.File]::ReadAllBytes($g)
        if ($a.Length -ne $b.Length -or [System.Linq.Enumerable]::SequenceEqual($a, $b) -eq $false) {
            "  DIFFERS: $($f.Name)"; $bad++
        }
    }
    foreach ($g in Get-ChildItem $gold -Filter "Game.*") {
        if (-not (Test-Path (Join-Path $run $g.Name))) { "  MISSING in run: $($g.Name)"; $bad++ }
    }
    if ($bad) { "output: $bad file(s) differ from golden"; exit 1 }
    "output: all {0} files identical to golden" -f $outputs.Count
} else {
    "output: no bench/golden to compare against (run once with -Golden)"
}
