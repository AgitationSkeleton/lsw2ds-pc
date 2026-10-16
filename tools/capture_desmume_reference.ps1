param(
    [int]$DelaySeconds = 26,
    [string]$Name = "reference"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$emulator = Join-Path $root "tools\vendor\desmume\DeSmuME_0.9.13_x64.exe"
$rom = Get-ChildItem -LiteralPath (Join-Path $root "rom") -Filter *.nds |
    Select-Object -First 1
$output = Join-Path $root "out\work\emulator_capture"

if (-not (Test-Path -LiteralPath $emulator)) {
    throw "Install DeSmuME under tools/vendor/desmume first."
}
if ($null -eq $rom) {
    throw "Place your own ROM under rom/ first."
}

New-Item -ItemType Directory -Force -Path $output | Out-Null
$process = Start-Process -FilePath $emulator -ArgumentList ('"' + $rom.FullName + '"') -WorkingDirectory (Split-Path -Parent $emulator) -PassThru

try {
    Start-Sleep -Seconds $DelaySeconds
    $process.Refresh()
    Add-Type -AssemblyName System.Drawing
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Lsw2CaptureWindow {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr handle, out RECT rectangle);
}
"@
    $rectangle = New-Object Lsw2CaptureWindow+RECT
    if (-not [Lsw2CaptureWindow]::GetWindowRect($process.MainWindowHandle, [ref]$rectangle)) {
        throw "Could not read the emulator window rectangle."
    }
    $width = $rectangle.Right - $rectangle.Left
    $height = $rectangle.Bottom - $rectangle.Top
    $bitmap = New-Object Drawing.Bitmap $width, $height
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rectangle.Left, $rectangle.Top, 0, 0, [Drawing.Size]::new($width, $height))
        $destination = Join-Path $output ($Name + ".png")
        $bitmap.Save($destination, [Drawing.Imaging.ImageFormat]::Png)
        Write-Output "Captured $destination ($width x $height)"
    }
    finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}
finally {
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
    }
}
