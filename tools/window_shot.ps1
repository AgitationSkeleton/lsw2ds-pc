# Capture the port's actual windows, including anything drawn over the game.
#
# WHY THIS AND NOT tools/pilot.py shot. The debug surface returns the GUEST
# framebuffer - what the DS rendered. That is the right thing for checking game
# output, and it is blind to everything the shell draws on top: the imgui
# settings window, the floating touch panel, the letterboxing, the title bar.
#
# Verifying the UI therefore needs a real window capture. PrintWindow(flag 2)
# asks the window to render itself, which works for hardware-composited windows
# where a plain screen grab gets a black rectangle.
#
#   powershell -File tools/window_shot.ps1                 # both windows
#   powershell -File tools/window_shot.ps1 -Match Top      # just one
#   powershell -File tools/window_shot.ps1 -OutDir shots

param(
    [string]$Match = "LEGO",
    [string]$OutDir = "out/work/logs"
)

$sig = @'
using System;using System.Text;using System.Drawing;using System.Runtime.InteropServices;
public struct WRect{public int L,T,R,B;}
public class WinShot{
 public delegate bool Cb(IntPtr h,IntPtr p);
 [DllImport("user32.dll")]public static extern bool EnumWindows(Cb c,IntPtr p);
 [DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out WRect r);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")]public static extern int GetWindowThreadProcessId(IntPtr h,out int pid);
 [DllImport("user32.dll")]public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
}
'@
Add-Type -TypeDefinition $sig -ReferencedAssemblies System.Drawing,System.Windows.Forms

$ids = @(Get-Process lsw2, nds_runner -ErrorAction SilentlyContinue |
         Select-Object -ExpandProperty Id)
if (-not $ids) { Write-Error "the port is not running"; exit 1 }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$hits = New-Object System.Collections.ArrayList
$cb = [WinShot+Cb]{
    param($h, $p)
    $wpid = 0
    [void][WinShot]::GetWindowThreadProcessId($h, [ref]$wpid)
    if ($ids -contains $wpid) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][WinShot]::GetWindowTextW($h, $sb, 256)
        $t = $sb.ToString()
        if ($t -and $t -like "*$Match*") { [void]$hits.Add(@($h, $t)) }
    }
    return $true
}
[void][WinShot]::EnumWindows($cb, [IntPtr]::Zero)

if ($hits.Count -eq 0) { Write-Error "no window matching '$Match'"; exit 1 }

foreach ($hit in $hits) {
    $h = $hit[0]; $title = $hit[1]
    $r = New-Object WRect
    [void][WinShot]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    if ($w -le 0 -or $ht -le 0) { Write-Warning "$title has no area"; continue }

    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    # Flag 2 is PW_RENDERFULLCONTENT: without it an accelerated window
    # captures as a black rectangle.
    [void][WinShot]::PrintWindow($h, $dc, 2)
    $g.ReleaseHdc($dc); $g.Dispose()

    $slug = if ($title -like "*Top*") { "top" } elseif ($title -like "*Touch*") { "touch" } else { "window" }
    $path = Join-Path $OutDir "window-$slug.png"
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    "captured {0}  ->  {1}  ({2}x{3})" -f $title, $path, $w, $ht
}
