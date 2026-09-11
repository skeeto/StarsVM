# Drive a window of the running Stars!VM process: type into a control and click a
# button, by message rather than by stealing the keyboard focus.
# Usage: powershell -File tools/poke.ps1 -Title "..." -Text "..." -Button "OK"
param([string]$Title = "", [string]$Text = "", [string]$Button = "", [string]$Click = "", [string]$Key = "", [int]$VKey = 0, [int]$Command = 0)
. (Join-Path $PSScriptRoot 'starsproc.ps1')

$src = @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class P {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RC r);
  public struct RC { public int l, t, r, b; }
}
"@
Add-Type -TypeDefinition $src

$pids = Get-StarsPids
if (-not $pids) { Write-Output (Get-StarsNotRunning); exit 1 }

$top = [IntPtr]::Zero
$cb = [P+EnumProc]{
  param($h, $l)
  $procId = 0
  [void][P]::GetWindowThreadProcessId($h, [ref]$procId)
  if ($pids -contains $procId -and [P]::IsWindowVisible($h)) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][P]::GetWindowTextW($h, $sb, 512)
    if ($sb.ToString() -like "*$Title*") { $script:top = $h }
  }
  return $true
}
[void][P]::EnumWindows($cb, [IntPtr]::Zero)
if ($top -eq [IntPtr]::Zero) { Write-Output "no window matching '$Title'"; exit 1 }

$kids = @()
$cb2 = [P+EnumProc]{
  param($h, $l)
  $cn = New-Object System.Text.StringBuilder 128
  [void][P]::GetClassNameW($h, $cn, 128)
  $tx = New-Object System.Text.StringBuilder 512
  [void][P]::GetWindowTextW($h, $tx, 512)
  $script:kids += [pscustomobject]@{ H = $h; C = $cn.ToString(); T = $tx.ToString() }
  return $true
}
[void][P]::EnumChildWindows($top, $cb2, [IntPtr]::Zero)
foreach ($k in $kids) {
  $r = New-Object P+RC
  [void][P]::GetWindowRect($k.H, [ref]$r)
  Write-Output ("  child {0} {1} '{2}' at {3},{4} {5}x{6}" -f
                $k.H, $k.C, $k.T, $r.l, $r.t, ($r.r - $r.l), ($r.b - $r.t))
}

[void][P]::SetForegroundWindow($top)

if ($Text -ne "") {
  $edit = $kids | Where-Object { $_.C -eq "Edit" } | Select-Object -First 1
  if (-not $edit) { Write-Output "no Edit control"; exit 1 }
  [void][P]::SendMessageW($edit.H, 0x0007, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_SETFOCUS
  foreach ($ch in $Text.ToCharArray()) {
    [void][P]::SendMessageW($edit.H, 0x0102, [IntPtr][int][char]$ch, [IntPtr]1) # WM_CHAR
  }
  Write-Output ("typed '{0}'" -f $Text)
}

# The splash screen paints its own buttons, so there is no child window to
# click - the game reads WM_LBUTTONDOWN on the main window instead.  lParam is a
# packed client-relative POINT in both Win16 and Win32, so no translation.
if ($Click -ne "") {
  $xy = $Click.Split(",")
  $lp = ([int]$xy[1] -shl 16) -bor ([int]$xy[0] -band 0xFFFF)
  [void][P]::PostMessageW($top, 0x0200, [IntPtr]::Zero, [IntPtr]$lp)   # WM_MOUSEMOVE
  [void][P]::PostMessageW($top, 0x0201, [IntPtr]1, [IntPtr]$lp)        # WM_LBUTTONDOWN
  Start-Sleep -Milliseconds 60
  [void][P]::PostMessageW($top, 0x0202, [IntPtr]::Zero, [IntPtr]$lp)   # WM_LBUTTONUP
  Write-Output ("clicked at {0}" -f $Click)
}

if ($VKey -ne 0) {
  [void][P]::PostMessageW($top, 0x0100, [IntPtr]$VKey, [IntPtr]1)       # WM_KEYDOWN
  Start-Sleep -Milliseconds 50
  [void][P]::PostMessageW($top, 0x0101, [IntPtr]$VKey, [IntPtr]1)       # WM_KEYUP
  Write-Output ("pressed vkey {0:X2}" -f $VKey)
}

if ($Command -ne 0) {
  [void][P]::PostMessageW($top, 0x0111, [IntPtr]$Command, [IntPtr]::Zero) # WM_COMMAND
  Write-Output ("sent command {0}" -f $Command)
}

if ($Key -ne "") {
  foreach ($ch in $Key.ToCharArray()) {
    [void][P]::PostMessageW($top, 0x0102, [IntPtr][int][char]$ch, [IntPtr]1)
  }
  Write-Output ("sent keys '{0}'" -f $Key)
}

if ($Button -ne "") {
  $b = $kids | Where-Object { $_.C -eq "Button" -and $_.T -replace '&','' -eq $Button } | Select-Object -First 1
  if (-not $b) { Write-Output "no button '$Button'"; exit 1 }
  [void][P]::PostMessageW($b.H, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)       # BM_CLICK
  Write-Output ("clicked {0}" -f $Button)
}
