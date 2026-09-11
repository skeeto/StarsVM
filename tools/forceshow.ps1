# Force-show a hidden top-level window of the stars16 process, by title.
param([string]$Title = "")
$src = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class FS {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public static List<IntPtr> Hits = new List<IntPtr>();
  public static List<string> Lines = new List<string>();
}
"@
Add-Type -TypeDefinition $src
$pids = @(Get-Process stars16 -ErrorAction SilentlyContinue | ForEach-Object { [uint32]$_.Id })
if (-not $pids) { Write-Output "not running"; exit 1 }
$cb = [FS+EnumProc]{
  param($h, $l)
  $procId = 0
  [void][FS]::GetWindowThreadProcessId($h, [ref]$procId)
  if ($pids -contains $procId) {
    $t = New-Object System.Text.StringBuilder 256; [void][FS]::GetWindowTextW($h, $t, 256)
    $c = New-Object System.Text.StringBuilder 128; [void][FS]::GetClassNameW($h, $c, 128)
    if ($t.ToString() -eq $Title) { [FS]::Hits.Add($h) }
    [FS]::Lines.Add(("{0} {1} '{2}' vis={3}" -f $h, $c.ToString(), $t.ToString(), [FS]::IsWindowVisible($h)))
  }
  return $true
}
[void][FS]::EnumWindows($cb, [IntPtr]::Zero)
[FS]::Lines | ForEach-Object { Write-Output $_ }
foreach ($h in [FS]::Hits) {
  [void][FS]::ShowWindow($h, 5)     # SW_SHOW
  [void][FS]::SetForegroundWindow($h)
  Write-Output ("forced SW_SHOW on {0}, now vis={1}" -f $h, [FS]::IsWindowVisible($h))
}
