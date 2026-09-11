# Capture a top-level window of the running Stars!VM process to a PNG.
# Usage: powershell -File tools/shot.ps1 out.png [titleSubstring]
param([string]$Out = "window.png", [string]$Match = "")
. (Join-Path $PSScriptRoot 'starsproc.ps1')

Add-Type -AssemblyName System.Drawing
$src = @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  public struct R { public int l, t, r, b; }
}
"@
Add-Type -TypeDefinition $src

$pids = Get-StarsPids
if (-not $pids) { Write-Output (Get-StarsNotRunning); exit 1 }

$found = @()
$cb = [W+EnumProc]{
  param($h, $l)
  $procId = 0
  [void][W]::GetWindowThreadProcessId($h, [ref]$procId)
  if ($pids -contains $procId -and [W]::IsWindowVisible($h)) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][W]::GetWindowTextW($h, $sb, 512)
    $script:found += [pscustomobject]@{ H = $h; T = $sb.ToString() }
  }
  return $true
}
[void][W]::EnumWindows($cb, [IntPtr]::Zero)

foreach ($w in $found) {
  $wr = New-Object W+R; [void][W]::GetWindowRect($w.H, [ref]$wr)
  $cr = New-Object W+R; [void][W]::GetClientRect($w.H, [ref]$cr)
  Write-Output ("window {0} '{1}' at {2},{3} size {4}x{5} client {6}x{7}" -f
                $w.H, $w.T, $wr.l, $wr.t, ($wr.r - $wr.l), ($wr.b - $wr.t), $cr.r, $cr.b)
}
$target = $found | Where-Object { $Match -eq "" -or $_.T -like "*$Match*" } | Select-Object -Last 1
if (-not $target) { Write-Output "no matching window"; exit 1 }

$r = New-Object W+R
[void][W]::GetWindowRect($target.H, [ref]$r)
$bmp = New-Object System.Drawing.Bitmap (($r.r - $r.l), ($r.b - $r.t))
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[void][W]::PrintWindow($target.H, $dc, 0)
$g.ReleaseHdc($dc)
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output ("saved {0} from '{1}'" -f $Out, $target.T)
