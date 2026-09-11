# Dump every window of the stars16 process with the attributes that decide
# taskbar presence, activation and caption icon.
$src = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class WI {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int i);
  [DllImport("user32.dll", EntryPoint="GetClassLongPtrW")] public static extern IntPtr GetClassLongPtrW(IntPtr h, int i);
  [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtrW(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint c);
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetActiveWindow();
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static List<string> Lines = new List<string>();
  public static uint[] Pids;
  static string Name(IntPtr h) {
    var c = new StringBuilder(128); GetClassNameW(h, c, 128);
    var t = new StringBuilder(256); GetWindowTextW(h, t, 256);
    return c.ToString() + " '" + t.ToString() + "'";
  }
  public static void Run(uint[] pids) {
    Pids = pids;
    EnumWindows((h, l) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (Array.IndexOf(Pids, pid) < 0) return true;
      long style = (uint)GetWindowLongW(h, -16);
      long ex    = (uint)GetWindowLongW(h, -20);
      IntPtr owner = GetWindow(h, 4);            // GW_OWNER
      IntPtr clsIcon = GetClassLongPtrW(h, -14); // GCLP_HICON
      IntPtr clsSm   = GetClassLongPtrW(h, -34); // GCLP_HICONSM
      IntPtr wBig = SendMessageW(h, 0x007F, (IntPtr)1, IntPtr.Zero); // WM_GETICON ICON_BIG
      IntPtr wSm  = SendMessageW(h, 0x007F, (IntPtr)0, IntPtr.Zero); // ICON_SMALL
      Lines.Add(string.Format(
        "{0} {1} style={2:X8} ex={3:X8} owner={4} clsIcon={5:X} clsSm={6:X} wmBig={7:X} wmSm={8:X} vis={9} enabled={10}",
        h, Name(h), style, ex, owner, (long)clsIcon, (long)clsSm, (long)wBig, (long)wSm,
        IsWindowVisible(h), IsWindowEnabled(h)));
      return true;
    }, IntPtr.Zero);
    Lines.Add("active=" + GetActiveWindow() + " foreground=" + GetForegroundWindow());
  }
}
"@
Add-Type -TypeDefinition $src
$pids = @(Get-Process stars16 -ErrorAction SilentlyContinue | ForEach-Object { [uint32]$_.Id })
if (-not $pids) { Write-Output "stars16 is not running"; exit 1 }
[WI]::Run($pids)
[WI]::Lines | ForEach-Object { Write-Output $_ }
