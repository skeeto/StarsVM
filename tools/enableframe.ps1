# During the tutorial hang: re-enable the frame from outside and watch the CPU.
$src = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class EF {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool EnableWindow(IntPtr h, bool e);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  public static List<IntPtr> Frames = new List<IntPtr>();
}
"@
Add-Type -TypeDefinition $src
$pids = @(Get-Process stars16 -ErrorAction SilentlyContinue | ForEach-Object { [uint32]$_.Id })
$cb = [EF+EnumProc]{
  param($h, $l)
  $procId = 0
  [void][EF]::GetWindowThreadProcessId($h, [ref]$procId)
  if ($pids -contains $procId) {
    $c = New-Object System.Text.StringBuilder 128; [void][EF]::GetClassNameW($h, $c, 128)
    if ($c.ToString() -eq 'starsframe') { [EF]::Frames.Add($h) }
  }
  return $true
}
[void][EF]::EnumWindows($cb, [IntPtr]::Zero)
if ([EF]::Frames.Count -eq 0) { Write-Output "no frame"; exit 1 }
$f = [EF]::Frames[0]
Write-Output ("frame {0} enabled={1}" -f $f, [EF]::IsWindowEnabled($f))
Write-Output ("cpu t0 = " + (Get-Process stars16).CPU)
Start-Sleep -Seconds 4
Write-Output ("cpu t4 = " + (Get-Process stars16).CPU)
[void][EF]::EnableWindow($f, $true)
Write-Output ("re-enabled; enabled={0}" -f [EF]::IsWindowEnabled($f))
Start-Sleep -Seconds 4
Write-Output ("cpu t8 = " + (Get-Process stars16).CPU)
Start-Sleep -Seconds 4
Write-Output ("cpu t12 = " + (Get-Process stars16).CPU)
