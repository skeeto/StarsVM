# Click at a client-relative point of a Stars!VM window using real mouse input,
# then put the pointer back where it was.
# Usage: powershell -File tools/click.ps1 -Title "Stars!" -X 80 -Y 430
param([string]$Title = "Stars!", [int]$X = 0, [int]$Y = 0, [switch]$WindowRelative)
. (Join-Path $PSScriptRoot 'starsproc.ps1')

$src = @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class C {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref PT p);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RC r);
  public struct RC { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out PT p);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct PT { public int x, y; }
}
"@
Add-Type -TypeDefinition $src

$pids = Get-StarsPids
if (-not $pids) { Write-Output (Get-StarsNotRunning); exit 1 }

$found = @()
$cb = [C+EnumProc]{
  param($h, $l)
  $procId = 0
  [void][C]::GetWindowThreadProcessId($h, [ref]$procId)
  if ($pids -contains $procId -and [C]::IsWindowVisible($h)) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][C]::GetWindowTextW($h, $sb, 512)
    if ($sb.ToString() -like "*$Title*") { $script:found += $h }
  }
  return $true
}
[void][C]::EnumWindows($cb, [IntPtr]::Zero)
if (-not $found) { Write-Output "no window matching '$Title'"; exit 1 }
$h = $found[-1]

$was = New-Object C+PT
[void][C]::GetCursorPos([ref]$was)

$p = New-Object C+PT
$p.x = $X; $p.y = $Y
if ($WindowRelative) {
  # Menu bars and captions live in the non-client area, which client
  # coordinates cannot address.
  $wr = New-Object C+RC
  [void][C]::GetWindowRect($h, [ref]$wr)
  $p.x = $wr.l + $X; $p.y = $wr.t + $Y
} else {
  [void][C]::ClientToScreen($h, [ref]$p)
}
[void][C]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 120
[void][C]::SetCursorPos($p.x, $p.y)
Start-Sleep -Milliseconds 80
[C]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)    # LEFTDOWN
Start-Sleep -Milliseconds 60
[C]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)    # LEFTUP
Start-Sleep -Milliseconds 150
[void][C]::SetCursorPos($was.x, $was.y)
Write-Output ("clicked {0},{1} (screen {2},{3})" -f $X, $Y, $p.x, $p.y)
