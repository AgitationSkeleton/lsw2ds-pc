# Click inside one of the port's windows, in CLIENT coordinates.
#
# WHY THIS EXISTS. The settings UI is imgui, drawn inside the game window, and
# the debug surface injects DS buttons and stylus taps - not host mouse events.
# So there was no way to press a button in our own UI from a script, which meant
# no way to verify that pressing it does what it claims. "It compiles" is not
# evidence, and this project has already shipped a panel that was correctly
# placed, correctly sized and entirely black.
#
# Coordinates are client-relative so they can be read straight off a capture
# from tools/window_shot.ps1 (minus its title-bar offset).
#
#   powershell -File tools/window_click.ps1 -Match Top -X 420 -Y 240

param(
    [string]$Match = "Top",
    [int]$X = 0,
    [int]$Y = 0
)

$sig = @'
using System;using System.Text;using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential)]public struct PT{public int X,Y;}
public class WinClick{
 public delegate bool Cb(IntPtr h,IntPtr p);
 [DllImport("user32.dll")]public static extern bool EnumWindows(Cb c,IntPtr p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")]public static extern int GetWindowThreadProcessId(IntPtr h,out int pid);
 [DllImport("user32.dll")]public static extern bool ClientToScreen(IntPtr h,ref PT p);
 [DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")]public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")]public static extern void mouse_event(uint f,uint x,uint y,uint d,IntPtr e);
}
'@
Add-Type -TypeDefinition $sig

$ids = @(Get-Process lsw2, nds_runner -ErrorAction SilentlyContinue |
         Select-Object -ExpandProperty Id)
if (-not $ids) { Write-Error "the port is not running"; exit 1 }

$found = [IntPtr]::Zero
$cb = [WinClick+Cb]{
    param($h, $p)
    $wpid = 0
    [void][WinClick]::GetWindowThreadProcessId($h, [ref]$wpid)
    if ($ids -contains $wpid) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][WinClick]::GetWindowTextW($h, $sb, 256)
        if ($sb.ToString() -like "*$Match*") { $script:found = $h; return $false }
    }
    return $true
}
[void][WinClick]::EnumWindows($cb, [IntPtr]::Zero)
if ($found -eq [IntPtr]::Zero) { Write-Error "no window matching '$Match'"; exit 1 }

$pt = New-Object PT; $pt.X = $X; $pt.Y = $Y
[void][WinClick]::ClientToScreen($found, [ref]$pt)
[void][WinClick]::SetForegroundWindow($found)
Start-Sleep -Milliseconds 200
[void][WinClick]::SetCursorPos($pt.X, $pt.Y)
Start-Sleep -Milliseconds 120
# imgui reacts on release, and needs to see the press in one frame and the
# release in a later one, so give it a gap rather than a synthetic instant click.
[void][WinClick]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)   # left down
Start-Sleep -Milliseconds 120
[void][WinClick]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)   # left up
Write-Output ("clicked client ({0},{1}) = screen ({2},{3})" -f $X, $Y, $pt.X, $pt.Y)
