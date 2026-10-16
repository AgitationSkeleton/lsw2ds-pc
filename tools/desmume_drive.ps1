<#
.SYNOPSIS
Drive DeSmuME with a scripted input sequence and capture its window.

.DESCRIPTION
Reference capture needs the game actually played: Bounty Hunter mode is
reachable from a fresh boot by walking to the arena, so no save state is
required. This sends synthetic keystrokes to the emulator window and grabs
frames, which is how the in-match HUD gets observed rather than guessed.

Steps are given as a string array and run in order:

    wait:<seconds>          idle
    tap:<key>[:<seconds>]   press and release (default 0.08s hold)
    hold:<key>:<seconds>    press, wait, release
    shot:<name>             capture the emulator window to <name>.png
    state:<slot>            save an emulator state into the given slot

Key names map to the DeSmuME 0.9.13 Windows defaults:
    a=X  b=Z  x=S  y=A  l=Q  r=W  start=Return  select=Backspace
    up/down/left/right = arrow keys

.NOTES
This takes over the foreground window and the keyboard while it runs. Do not
type during a run.
#>
param(
    [Parameter(Mandatory = $true)][string[]]$Steps,
    [string]$OutputDir = "out\work\emulator_capture",
    [switch]$KeepOpen,
    [int]$BootSeconds = 14,
    [int]$BorderX = 6,
    [int]$Toolbar = 25,
    [switch]$Attach
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$emulator = Join-Path $root "tools\vendor\desmume\DeSmuME_0.9.13_x64.exe"
$rom = Get-ChildItem -LiteralPath (Join-Path $root "rom") -Filter *.nds | Select-Object -First 1
$output = Join-Path $root $OutputDir

if (-not (Test-Path -LiteralPath $emulator)) { throw "DeSmuME missing under tools/vendor/desmume." }
if ($null -eq $rom) { throw "Place your own ROM under rom/ first." }
New-Item -ItemType Directory -Force -Path $output | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Lsw2Input {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref System.Drawing.Point p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
}
"@ -ReferencedAssemblies System.Drawing, System.Drawing.Primitives

$keyMap = @{
    'a' = 0x58; 'b' = 0x5A; 'x' = 0x53; 'y' = 0x41
    'l' = 0x51; 'r' = 0x57; 'start' = 0x0D; 'select' = 0x08
    'up' = 0x26; 'down' = 0x28; 'left' = 0x25; 'right' = 0x27
    'space' = 0x20; 'esc' = 0x1B
}
foreach ($slot in 0..9) { $keyMap["num$slot"] = 0x30 + $slot }
# DeSmuME 0.9.13 binds load-state to F1..F10 and save-state to Shift+F1..F10.
foreach ($index in 1..10) { $keyMap["f$index"] = 0x6F + $index }

function Send-Key([int]$vk, [bool]$down) {
    $scan = [Lsw2Input]::MapVirtualKey([uint32]$vk, 0)
    $flags = 0x0008  # KEYEVENTF_SCANCODE
    if (-not $down) { $flags = $flags -bor 0x0002 }
    [Lsw2Input]::keybd_event([byte]$vk, [byte]$scan, [uint32]$flags, [UIntPtr]::Zero)
}

if ($Attach) {
    $process = Get-Process -Name 'DeSmuME*' -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($null -eq $process) { throw "No running DeSmuME to attach to." }
    $handle = $process.MainWindowHandle
    [void][Lsw2Input]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 500
} else {
    $process = Start-Process -FilePath $emulator -ArgumentList ('"' + $rom.FullName + '"') `
        -WorkingDirectory (Split-Path -Parent $emulator) -PassThru
    Start-Sleep -Seconds 3
    $process.Refresh()
    $handle = $process.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { throw "DeSmuME window did not appear." }
    [void][Lsw2Input]::ShowWindow($handle, 9)
    [void][Lsw2Input]::SetForegroundWindow($handle)
    Start-Sleep -Seconds $BootSeconds
}

function Save-Shot([string]$name) {
    $process.Refresh()
    $rect = New-Object Lsw2Input+RECT
    [void][Lsw2Input]::GetClientRect($handle, [ref]$rect)
    $origin = New-Object System.Drawing.Point 0, 0
    [void][Lsw2Input]::ClientToScreen($handle, [ref]$origin)
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { throw "Bad client rect for capture." }
    $bmp = New-Object Drawing.Bitmap $w, $h
    $g = [Drawing.Graphics]::FromImage($bmp)
    try {
        $g.CopyFromScreen($origin.X, $origin.Y, 0, 0, [Drawing.Size]::new($w, $h))
        $path = Join-Path $output ($name + ".png")
        $bmp.Save($path, [Drawing.Imaging.ImageFormat]::Png)
        Write-Output "SHOT $path ${w}x${h}"
    } finally { $g.Dispose(); $bmp.Dispose() }
}

try {
    foreach ($step in $Steps) {
        $parts = $step.Split(':')
        $verb = $parts[0].ToLower()
        switch ($verb) {
            'wait' { Start-Sleep -Milliseconds ([double]$parts[1] * 1000) }
            'shot' { Save-Shot $parts[1] }
            'tap' {
                $vk = $keyMap[$parts[1].ToLower()]
                if ($null -eq $vk) { throw "Unknown key '$($parts[1])'" }
                $hold = if ($parts.Count -gt 2) { [double]$parts[2] } else { 0.08 }
                [void][Lsw2Input]::SetForegroundWindow($handle)
                Send-Key $vk $true
                Start-Sleep -Milliseconds ($hold * 1000)
                Send-Key $vk $false
                Start-Sleep -Milliseconds 120
            }
            'hold' {
                $vk = $keyMap[$parts[1].ToLower()]
                if ($null -eq $vk) { throw "Unknown key '$($parts[1])'" }
                [void][Lsw2Input]::SetForegroundWindow($handle)
                Send-Key $vk $true
                Start-Sleep -Milliseconds ([double]$parts[2] * 1000)
                Send-Key $vk $false
                Start-Sleep -Milliseconds 120
            }
            'touch' {
                # touch:<x>:<y>[:<seconds>] in bottom-screen 256x192 coordinates.
                $process.Refresh()
                $rect = New-Object Lsw2Input+RECT
                [void][Lsw2Input]::GetClientRect($handle, [ref]$rect)
                $origin = New-Object System.Drawing.Point 0, 0
                [void][Lsw2Input]::ClientToScreen($handle, [ref]$origin)
                $sx = $origin.X + $BorderX + [int]$parts[1]
                $sy = $origin.Y + $Toolbar + 192 + [int]$parts[2]
                $hold = if ($parts.Count -gt 3) { [double]$parts[3] } else { 0.12 }
                [void][Lsw2Input]::SetForegroundWindow($handle)
                [void][Lsw2Input]::SetCursorPos($sx, $sy)
                Start-Sleep -Milliseconds 60
                [Lsw2Input]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
                Start-Sleep -Milliseconds ($hold * 1000)
                [Lsw2Input]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
                Start-Sleep -Milliseconds 200
                Write-Output "TOUCH $($parts[1]),$($parts[2])"
            }
            'state' {
                # Shift+F<slot> saves a state; F<slot> alone loads it.
                $vk = $keyMap["f$($parts[1])"]
                [void][Lsw2Input]::SetForegroundWindow($handle)
                Send-Key 0x10 $true
                Send-Key $vk $true
                Start-Sleep -Milliseconds 80
                Send-Key $vk $false
                Send-Key 0x10 $false
                Start-Sleep -Milliseconds 400
                Write-Output "STATE saved slot $($parts[1])"
            }
            default { throw "Unknown step '$step'" }
        }
    }
} finally {
    if (-not $KeepOpen -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
