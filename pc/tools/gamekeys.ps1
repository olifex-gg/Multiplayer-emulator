# gamekeys.ps1 - drive a running AnimalCrossing.exe from a script, for playtesting
# on the user's PC (see docs/HANDOFF.md, "Playtesting without a friend").
#
#   gamekeys.ps1 ptap  <key> [holdMs] [n] [gapMs]  press a key n times (posted to the window)
#   gamekeys.ps1 phold <key> <ms>                  hold a key for ms (posted)
#   gamekeys.ps1 shot  <out.png>                   screenshot just the game window
#   gamekeys.ps1 move  <x> <y> <w> <h>             move/resize the window (clear of other windows)
#   gamekeys.ps1 focus                             try to bring the game to the front
#   gamekeys.ps1 ptype <text>                      type text (letters, digits, spaces; posted), e.g. a chat line
#   gamekeys.ps1 tap / hold                        like ptap/phold but via SendInput; needs focus
#
# ptap/phold post WM_KEYDOWN/WM_KEYUP straight to the game window, which SDL2 turns
# into ordinary key events, so they work while the game is in the background --
# Windows refuses SetForegroundWindow from a script, so `tap`/`hold` usually do not.
# Key names follow keybindings.ini defaults: Space=A, LShift=B, Return=Start, X, Y, Z,
# Q=L, E=R, W/A/S/D=stick, Up/Down/Left/Right=C-stick, I/J/K/L=D-pad, Escape.
# A POSTED Left Shift never reaches the game as B: SDL treats Shift specially on
# Windows (it polls the real key state for modifiers). Rebind B to C in the test
# copy's keybindings.ini (`b = C`) and press C here instead.
param([string]$cmd, [string]$a1, [string]$a2, [string]$a3, [string]$a4)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$sig = @'
using System;
using System.Runtime.InteropServices;
public static class GK {
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Explicit)] public struct INPUTUNION { [FieldOffset(0)] public KEYBDINPUT ki; [FieldOffset(0)] public long pad1; [FieldOffset(8)] public long pad2; [FieldOffset(16)] public long pad3; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public INPUTUNION u; }
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint flags);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
  public static void Post(IntPtr h, int vk, int scan, bool extended, bool up) {
    long l = 1 | ((long)scan << 16) | (extended ? (1L << 24) : 0);
    if (up) l |= (1L << 30) | (1L << 31);
    PostMessage(h, up ? 0x0101u : 0x0100u, (IntPtr)vk, (IntPtr)l);
  }
  public static void Key(ushort scan, bool extended, bool up) {
    INPUT[] inp = new INPUT[1];
    inp[0].type = 1;
    inp[0].u.ki.wScan = scan;
    inp[0].u.ki.dwFlags = 0x0008 | (extended ? 0x0001u : 0u) | (up ? 0x0002u : 0u);
    SendInput(1, inp, Marshal.SizeOf(typeof(INPUT)));
  }
}
'@
if (-not ("GK" -as [type])) { Add-Type -TypeDefinition $sig }
# Work in physical pixels: on a scaled display, window rects and screenshots
# otherwise disagree about where the window is.
[GK]::SetProcessDPIAware() | Out-Null
$scan = @{ Space=0x39; Return=0x1C; LShift=0x2A; X=0x2D; Y=0x15; Z=0x2C; Q=0x10; E=0x12; W=0x11; A=0x1E; S=0x1F; D=0x20;
           I=0x17; J=0x24; K=0x25; L=0x26; Escape=0x01; Up=0x48; Down=0x50; Left=0x4B; Right=0x4D; T=0x14; Back=0x0E; C=0x2E; Tab=0x0F }
$ext = @{ Up=$true; Down=$true; Left=$true; Right=$true }
$vk = @{ Space=0x20; Return=0x0D; LShift=0x10; X=0x58; Y=0x59; Z=0x5A; Q=0x51; E=0x45; W=0x57; A=0x41; S=0x53; D=0x44;
         I=0x49; J=0x4A; K=0x4B; L=0x4C; Escape=0x1B; Up=0x26; Down=0x28; Left=0x25; Right=0x27; T=0x54; Back=0x08; C=0x43; Tab=0x09 }
function PType([string]$text) {
  $p = Get-Game; if (-not $p) { throw "game window not found" }
  foreach ($ch in $text.ToCharArray()) {
    $v = [int][char]::ToUpper($ch)            # letters, digits and space are their own virtual keys
    $sc = [GK]::MapVirtualKey([uint32]$v, 0)
    [GK]::Post($p.MainWindowHandle, $v, [int]$sc, $false, $false); Start-Sleep -Milliseconds 40
    [GK]::Post($p.MainWindowHandle, $v, [int]$sc, $false, $true);  Start-Sleep -Milliseconds 60
  }
}
function PTap([string]$k, [int]$hold, [int]$n, [int]$gap) {
  $p = Get-Game; if (-not $p) { throw "game window not found" }
  for ($i = 0; $i -lt $n; $i++) {
    [GK]::Post($p.MainWindowHandle, $vk[$k], $scan[$k], [bool]$ext[$k], $false); Start-Sleep -Milliseconds $hold
    [GK]::Post($p.MainWindowHandle, $vk[$k], $scan[$k], [bool]$ext[$k], $true);  Start-Sleep -Milliseconds $gap
  }
}
# With several games running (two residents on one PC), set $env:GAMEKEYS_PID to pick one.
function Get-Game {
  $all = Get-Process -Name AnimalCrossing -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }
  if ($env:GAMEKEYS_PID) { return $all | Where-Object { $_.Id -eq [int]$env:GAMEKEYS_PID } | Select-Object -First 1 }
  return $all | Select-Object -First 1
}
function Focus-Game {
  $p = Get-Game; if (-not $p) { throw "game window not found" }
  $h = $p.MainWindowHandle
  $fg = [GK]::GetForegroundWindow()
  $fgThread = [GK]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
  $me = [GK]::GetCurrentThreadId()
  if ($fgThread -ne 0) { [GK]::AttachThreadInput($me, $fgThread, $true) | Out-Null }
  [GK]::ShowWindow($h, 9) | Out-Null
  [GK]::BringWindowToTop($h) | Out-Null
  [GK]::SetForegroundWindow($h) | Out-Null
  if ($fgThread -ne 0) { [GK]::AttachThreadInput($me, $fgThread, $false) | Out-Null }
  Start-Sleep -Milliseconds 300
  return $p
}
function Tap([string]$k, [int]$hold, [int]$n, [int]$gap) {
  for ($i = 0; $i -lt $n; $i++) {
    [GK]::Key($scan[$k], [bool]$ext[$k], $false); Start-Sleep -Milliseconds $hold
    [GK]::Key($scan[$k], [bool]$ext[$k], $true);  Start-Sleep -Milliseconds $gap
  }
}
switch ($cmd) {
  'focus' { $p = Focus-Game; "focused pid $($p.Id) fg=$([GK]::GetForegroundWindow() -eq $p.MainWindowHandle)" }
  'move'  { $p = Get-Game; [GK]::SetWindowPos($p.MainWindowHandle, [IntPtr]::Zero, [int]$a1, [int]$a2, [int]$a3, [int]$a4, 0x0040) | Out-Null; "moved to $a1,$a2 size $a3 x $a4" }
  'tap'   { Focus-Game | Out-Null; $h = if ($a2) { [int]$a2 } else { 80 }; $n = if ($a3) { [int]$a3 } else { 1 }; $g = if ($a4) { [int]$a4 } else { 250 }; Tap $a1 $h $n $g; "tapped $a1 x$n" }
  'ptap'  { $h = if ($a2) { [int]$a2 } else { 80 }; $n = if ($a3) { [int]$a3 } else { 1 }; $g = if ($a4) { [int]$a4 } else { 250 }; PTap $a1 $h $n $g; "posted $a1 x$n" }
  'ptype' { PType $a1; "typed '$a1'" }
  'phold' { $p = Get-Game; [GK]::Post($p.MainWindowHandle, $vk[$a1], $scan[$a1], [bool]$ext[$a1], $false); Start-Sleep -Milliseconds ([int]$a2); [GK]::Post($p.MainWindowHandle, $vk[$a1], $scan[$a1], [bool]$ext[$a1], $true); "posted hold $a1 for $a2 ms" }
  'pdown' { $p = Get-Game; [GK]::Post($p.MainWindowHandle, $vk[$a1], $scan[$a1], [bool]$ext[$a1], $false); "posted $a1 down" }
  'pup'   { $p = Get-Game; [GK]::Post($p.MainWindowHandle, $vk[$a1], $scan[$a1], [bool]$ext[$a1], $true); "posted $a1 up" }
  'hold'  { Focus-Game | Out-Null; [GK]::Key($scan[$a1], [bool]$ext[$a1], $false); Start-Sleep -Milliseconds ([int]$a2); [GK]::Key($scan[$a1], [bool]$ext[$a1], $true); "held $a1 for $a2 ms" }
  'shot'  {
    $p = Get-Game; if (-not $p) { throw "game window not found" }
    $r = New-Object GK+RECT; [GK]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $h = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $bmp.Save($a1, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
    "saved $a1 ($w x $h at $($r.L),$($r.T))"
  }
  default { "usage: focus | tap key [hold n gap] | hold key ms | shot out.png" }
}
