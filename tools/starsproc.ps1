# starsproc.ps1 - find the running emulator, whatever its binary is called.
#
# Dot-source this; there is nothing to run.  Every other script here needs the
# same two things - the process, or just its pid - and every one of them used to
# open with `Get-Process stars16`.  When the project settled on Stars!VM and the
# Makefile's TARGET changed, all nine of them went on cheerfully reporting
# "stars16 is not running" against the only binary the tree still builds, which
# is the most useless possible failure: it looks exactly like the emulator
# having exited.
#
# So nothing below writes the name down.  It comes from the Makefile, which is
# where TARGET is decided, and the fallback matches the two window classes -
# starsframe and starstitle - that Stars! itself registers, so they outlive any
# renaming of ours.

# The Makefile is the single source of truth for what gets built.  Returns the
# base name a process would carry (no .exe), or $null if it cannot be read.
function Get-StarsName {
    $mk = Join-Path (Split-Path $PSScriptRoot -Parent) 'Makefile'
    if (Test-Path $mk) {
        $m = Select-String -Path $mk -Pattern '^\s*TARGET\s*:?=\s*(\S.*?)\s*$' |
             Select-Object -First 1
        if ($m) {
            return [IO.Path]::GetFileNameWithoutExtension($m.Matches[0].Groups[1].Value)
        }
    }
    return $null
}

# The emulator's processes, or an empty array.  Matched on ProcessName rather
# than passed to -Name, because -Name reads its argument as a wildcard, and the
# name here comes out of the Makefile: whatever TARGET says is one punctuation
# change away from a pattern rather than a literal.
function Get-StarsProcess {
    $name = Get-StarsName
    if ($name) {
        $p = @(Get-Process -ErrorAction SilentlyContinue |
               Where-Object { $_.ProcessName -eq $name })
        if ($p) { return $p }
    }

    # Nothing under that name: ask the windows instead.  Only worth compiling
    # the interop for when the cheap answer has already failed.
    if (-not ('StarsFind' -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class StarsFind {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);

  public static List<uint> Pids = new List<uint>();

  public static void ByClass() {
    Pids.Clear();
    EnumWindows((h, l) => {
      var c = new StringBuilder(64); GetClassNameW(h, c, 64);
      var s = c.ToString();
      if (s == "starsframe" || s == "starstitle") {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (!Pids.Contains(pid)) Pids.Add(pid);
      }
      return true;
    }, IntPtr.Zero);
  }
}
"@
    }
    [StarsFind]::ByClass()
    return @([StarsFind]::Pids |
             ForEach-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
}

function Get-StarsPids {
    return @(Get-StarsProcess | ForEach-Object { [uint32]$_.Id })
}

# What to say when there is nothing to talk to.
function Get-StarsNotRunning {
    $n = Get-StarsName
    if ($n) { return "$n is not running" }
    return "the emulator is not running"
}
