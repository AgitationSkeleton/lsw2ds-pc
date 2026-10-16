# Resize one of the port's windows from outside it.
#
# WHY THIS EXISTS. Adaptive widescreen only engages when the gameplay window is
# wider than 4:3, and the width it picks comes from the window's shape. Testing
# it therefore needs a window of a chosen shape, and neither the settings file
# nor the presets can produce one - they only offer whole multiples of 256x192,
# which are all 4:3 by construction. Dragging the corner by hand is not a thing
# an automated check can do.
#
# The client area is what matters, so the size asked for is the client size and
# the border is added on top; setting the outer rect instead would silently
# under-size the picture by the frame thickness and quietly change the aspect
# the widescreen code sees.
#
#   powershell -File tools/window_resize.ps1 -Match Top -Width 1600 -Height 600

param(
    [string]$Match = "Top",
    [int]$Width = 1600,
    [int]$Height = 600
)

$sig = @'
using System;using System.Text;using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential)]public struct RECT{public int L,T,R,B;}
public class WinMove{
 public delegate bool Cb(IntPtr h,IntPtr p);
 [DllImport("user32.dll")]public static extern bool EnumWindows(Cb c,IntPtr p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")]public static extern int GetWindowThreadProcessId(IntPtr h,out int pid);
 [DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")]public static extern bool GetClientRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")]public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int t,bool repaint);
 [DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
}
'@
Add-Type -TypeDefinition $sig

# Both binaries, matching window_shot.ps1: nds_runner is the framework's own
# runner, used as a reference to compare our shell against.
$ids = @(Get-Process lsw2, nds_runner -ErrorAction SilentlyContinue |
         Select-Object -ExpandProperty Id)
if (-not $ids) { Write-Error "the port is not running"; exit 1 }

$found = [IntPtr]::Zero
$title = ""
$cb = [WinMove+Cb]{
    param($h, $p)
    $wpid = 0
    [void][WinMove]::GetWindowThreadProcessId($h, [ref]$wpid)
    if ($ids -contains $wpid) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][WinMove]::GetWindowTextW($h, $sb, 256)
        $t = $sb.ToString()
        if ($t -and $t -like "*$Match*") {
            $script:found = $h
            $script:title = $t
            return $false
        }
    }
    return $true
}
[void][WinMove]::EnumWindows($cb, [IntPtr]::Zero)
if ($found -eq [IntPtr]::Zero) { Write-Error "no window matching '$Match'"; exit 1 }

# Border thickness = outer rect minus client rect, measured rather than assumed
# so this keeps working under a different DPI or theme.
$outer = New-Object RECT; $client = New-Object RECT
[void][WinMove]::GetWindowRect($found, [ref]$outer)
[void][WinMove]::GetClientRect($found, [ref]$client)
$padW = ($outer.R - $outer.L) - ($client.R - $client.L)
$padH = ($outer.B - $outer.T) - ($client.B - $client.T)

[void][WinMove]::MoveWindow($found, $outer.L, $outer.T, $Width + $padW, $Height + $padH, $true)
[void][WinMove]::SetForegroundWindow($found)
Write-Output ("resized '{0}' client to {1}x{2} (frame {3}x{4})" -f $title, $Width, $Height, $padW, $padH)
