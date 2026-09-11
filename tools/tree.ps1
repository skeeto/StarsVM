# Print the whole window tree of the stars16 process, visible or not.
$src = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class T {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RC r);
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
  public struct RC { public int l, t, r, b; }

  public static List<string> Lines = new List<string>();
  public static uint[] Pids;

  static string Describe(IntPtr h, int depth) {
    var c = new StringBuilder(128); GetClassNameW(h, c, 128);
    var t = new StringBuilder(256); GetWindowTextW(h, t, 256);
    RC r; GetWindowRect(h, out r);
    return new string(' ', depth * 2) + string.Format("{0} {1} '{2}' {3},{4} {5}x{6}{7}",
      h, c, t, r.l, r.t, r.r - r.l, r.b - r.t, IsWindowVisible(h) ? "" : " HIDDEN");
  }

  public static void Walk(IntPtr h, int depth) {
    Lines.Add(Describe(h, depth));
    EnumChildWindows(h, (c, l) => {
      if (GetParent(c) == h) Walk(c, depth + 1);
      return true;
    }, IntPtr.Zero);
  }

  public static void Run(uint[] pids) {
    Pids = pids;
    EnumWindows((h, l) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (Array.IndexOf(Pids, pid) >= 0) Walk(h, 0);
      return true;
    }, IntPtr.Zero);
  }
}
"@
Add-Type -TypeDefinition $src
$pids = @(Get-Process stars16 -ErrorAction SilentlyContinue | ForEach-Object { [uint32]$_.Id })
if (-not $pids) { Write-Output "stars16 is not running"; exit 1 }
[T]::Run($pids)
[T]::Lines | ForEach-Object { Write-Output $_ }
