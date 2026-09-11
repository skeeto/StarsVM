# Which windows of the stars16 process have a non-empty update region right now?
$src = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class UR {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetUpdateRect(IntPtr h, out RC r, bool erase);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public struct RC { public int l, t, r, b; }
  public static List<string> Lines = new List<string>();
  public static uint[] Pids;
  static void Probe(IntPtr h) {
    RC r;
    bool dirty = GetUpdateRect(h, out r, false);
    var c = new StringBuilder(128); GetClassNameW(h, c, 128);
    var t = new StringBuilder(128); GetWindowTextW(h, t, 128);
    if (dirty)
      Lines.Add(string.Format("DIRTY {0} {1} '{2}' update={3},{4}-{5},{6} vis={7}",
                              h, c, t, r.l, r.t, r.r, r.b, IsWindowVisible(h)));
  }
  public static void Run(uint[] pids) {
    Pids = pids;
    EnumWindows((h, l) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (Array.IndexOf(Pids, pid) < 0) return true;
      Probe(h);
      EnumChildWindows(h, (c2, l2) => { Probe(c2); return true; }, IntPtr.Zero);
      return true;
    }, IntPtr.Zero);
  }
}
"@
Add-Type -TypeDefinition $src
$pids = @(Get-Process stars16 -ErrorAction SilentlyContinue | ForEach-Object { [uint32]$_.Id })
if (-not $pids) { Write-Output "not running"; exit 1 }
for ($i = 0; $i -lt 4; $i++) {
  [UR]::Lines.Clear()
  [UR]::Run($pids)
  Write-Output ("--- sample {0}: {1} dirty ---" -f $i, [UR]::Lines.Count)
  [UR]::Lines | ForEach-Object { Write-Output $_ }
  Start-Sleep -Milliseconds 700
}
