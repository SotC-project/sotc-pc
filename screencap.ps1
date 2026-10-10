# Captures the primary screen to a PNG, with the mouse pointer drawn in (CopyFromScreen alone
# leaves it out). -MoveTo x,y first puts the pointer there.
# Usage: pwsh -File screencap.ps1 -Out shot.png [-MoveTo 960,540]
param([string]$Out, [int[]]$MoveTo)

Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Cap {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
    [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO {
        public int cbSize, flags; public IntPtr hCursor; public POINT pt;
    }
    [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO info);
    [DllImport("user32.dll")] public static extern bool DrawIconEx(IntPtr dc, int x, int y,
        IntPtr icon, int w, int h, int step, IntPtr brush, int flags);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@

# Work in real pixels, or a scaled desktop gives a cropped picture and wrong positions.
[void][Cap]::SetProcessDPIAware()
if ($MoveTo) {
    [void][Cap]::SetCursorPos($MoveTo[0], $MoveTo[1])
    Start-Sleep -Milliseconds 700
}
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$info = New-Object Cap+CURSORINFO
$info.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($info)
if ([Cap]::GetCursorInfo([ref]$info) -and ($info.flags -band 1)) {
    $dc = $graphics.GetHdc()
    [void][Cap]::DrawIconEx($dc, $info.pt.x - $bounds.X, $info.pt.y - $bounds.Y, $info.hCursor, 0, 0, 0, [IntPtr]::Zero, 3)
    $graphics.ReleaseHdc($dc)
    Write-Host "pointer visible at $($info.pt.x),$($info.pt.y)"
} else {
    Write-Host "pointer hidden"
}
$bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "$($bounds.Width)x$($bounds.Height) saved"
