<#
.SYNOPSIS
Capture the window of an already-running DeSmuME without sending any input.

.DESCRIPTION
Used when a human is driving the emulator. This never steals focus and never
synthesises keystrokes, so it is safe to run while someone is playing. Pass
-Repeat/-IntervalSeconds to grab a burst, which is how a live match gets
sampled across several frames.
#>
param(
    [string]$Name = "live",
    [string]$OutputDir = "out\work\emulator_capture",
    [int]$Repeat = 1,
    [double]$IntervalSeconds = 1.0
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$output = Join-Path $root $OutputDir
New-Item -ItemType Directory -Force -Path $output | Out-Null

$process = Get-Process -Name 'DeSmuME*' -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if ($null -eq $process) { throw "No running DeSmuME window found. Launch it first." }

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Lsw2Grab {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref System.Drawing.Point p);
}
"@ -ReferencedAssemblies System.Drawing, System.Drawing.Primitives

$handle = $process.MainWindowHandle
for ($index = 0; $index -lt $Repeat; $index++) {
    $rect = New-Object Lsw2Grab+RECT
    [void][Lsw2Grab]::GetClientRect($handle, [ref]$rect)
    $origin = New-Object System.Drawing.Point 0, 0
    [void][Lsw2Grab]::ClientToScreen($handle, [ref]$origin)
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { throw "Bad client rect; is the window minimised?" }
    $bmp = New-Object Drawing.Bitmap $w, $h
    $g = [Drawing.Graphics]::FromImage($bmp)
    try {
        $g.CopyFromScreen($origin.X, $origin.Y, 0, 0, [Drawing.Size]::new($w, $h))
        $suffix = if ($Repeat -gt 1) { "_{0:d2}" -f $index } else { "" }
        $path = Join-Path $output ($Name + $suffix + ".png")
        $bmp.Save($path, [Drawing.Imaging.ImageFormat]::Png)
        Write-Output "SHOT $path ${w}x${h}"
    } finally { $g.Dispose(); $bmp.Dispose() }
    if ($index -lt $Repeat - 1) { Start-Sleep -Seconds $IntervalSeconds }
}
