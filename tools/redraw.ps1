# Force the starved children of the stars16 windows to paint, and see whether
# the repaint storm stops.
$src = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class RW {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr rgn, uint flags);
  [DllImport("user32.dll")] public static extern bool GetUpdateRect(IntPtr h, out RC r, bool erase);
  public struct RC { public int l, t, r, b; }
  public static List<IntPtr> Tops = new List<IntPtr>();
}
"@
Add-Type -TypeDefinition $src
$pids = @(Get-Process stars16 -ErrorAction SilentlyContinue | ForEach-Object { [uint32]$_.Id })
$cb = [RW+EnumProc]{
  param($h, $l)
  $procId = 0
  [void][RW]::GetWindowThreadProcessId($h, [ref]$procId)
  if ($pids -contains $procId) {
    $c = New-Object System.Text.StringBuilder 128; [void][RW]::GetClassNameW($h, $c, 128)
    if ($c.ToString() -eq 'starsframe') { [RW]::Tops.Add($h) }
  }
  return $true
}
[void][RW]::EnumWindows($cb, [IntPtr]::Zero)
if ([RW]::Tops.Count -eq 0) { Write-Output "no frame"; exit 1 }
$f = [RW]::Tops[0]
$a = (Get-Process stars16).CPU
Start-Sleep -Seconds 3
$b = (Get-Process stars16).CPU
Write-Output ("before: " + [math]::Round($b-$a,2) + "s per 3s")
# RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW|RDW_ALLCHILDREN = 1|4|0x100|0x80
[void][RW]::RedrawWindow($f, [IntPtr]::Zero, [IntPtr]::Zero, 0x185)
Start-Sleep -Seconds 3
$c2 = (Get-Process stars16).CPU
Write-Output ("after RedrawWindow(ALLCHILDREN|UPDATENOW): " + [math]::Round($c2-$b,2) + "s per 3s")
Start-Sleep -Seconds 3
$d = (Get-Process stars16).CPU
Write-Output ("3s later: " + [math]::Round($d-$c2,2) + "s per 3s")
