param(
    [int]$DelaySeconds = 26,
    [int]$RecordSeconds = 20,
    [string]$Name = "title_reference"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$emulator = Join-Path $root "tools/vendor/desmume/DeSmuME_0.9.13_x64.exe"
$rom = Get-ChildItem -LiteralPath (Join-Path $root "rom") -Filter *.nds |
    Select-Object -First 1
$output = Join-Path $root "out\work\emulator_audio_ab"
$destination = Join-Path $output ($Name + ".wav")

if ($null -eq $rom) { throw "Place your own ROM under rom/ first." }
New-Item -ItemType Directory -Force -Path $output | Out-Null

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Lsw2DesmumeAudio {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@

$process = Start-Process -FilePath $emulator -ArgumentList ('"' + $rom.FullName + '"') `
    -WorkingDirectory (Split-Path -Parent $emulator) -PassThru
try {
    Start-Sleep -Seconds $DelaySeconds
    $process.Refresh()
    if ($process.MainWindowHandle -eq 0) { throw "DeSmuME window was not ready." }
    # DeSmuME 0.9.13 File | Record WAV is WM_COMMAND 40017. The native save
    # dialog is then filled with a project-local absolute path.
    [void][Lsw2DesmumeAudio]::PostMessage($process.MainWindowHandle, 0x0111, 40017, 0)
    Start-Sleep -Milliseconds 500
    Add-Type -AssemblyName System.Windows.Forms
    [Windows.Forms.SendKeys]::SendWait($destination)
    [Windows.Forms.SendKeys]::SendWait("{ENTER}")
    Start-Sleep -Seconds $RecordSeconds
    [void][Lsw2DesmumeAudio]::PostMessage($process.MainWindowHandle, 0x0111, 40017, 0)
    Start-Sleep -Milliseconds 500
}
finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}

if (-not (Test-Path -LiteralPath $destination)) {
    throw "DeSmuME did not create $destination"
}
Write-Output "Captured exact emulated WAV: $destination"
