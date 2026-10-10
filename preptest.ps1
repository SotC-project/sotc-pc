# Start timing with the window visible, in the test profile.
# Usage: pwsh -File preptest.ps1 -Max 90 [-Prep ask|prepare|skip] [-Shot file.png -ShotAt 6]
#        [-Key Return -KeyAt 8] [-Shot2 file.png -Shot2At 14] [-Stay 20] [-Threads 0] [-Build Release]
# Prints the wall time from launch to the game's first frame.
param([int]$Max = 90, [string]$Prep = '', [string]$Shot = '', [double]$ShotAt = 6,
    [string]$Key = '', [double]$KeyAt = 8, [string]$Shot2 = '', [double]$Shot2At = 14,
    [int]$Stay = 0, [string]$Threads = '', [string]$Build = 'RelWithDebInfo', [switch]$Fullscreen)

if (Get-Process shadps4, ShadowOfTheColossus, i3 -ErrorAction SilentlyContinue) { "REFUSED: a game instance is running"; return }
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class PK {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
}
'@
$dir = Join-Path $PSScriptRoot 'testrun'
$out = Join-Path $dir 'stdout.txt'
Remove-Item $out -ErrorAction SilentlyContinue
$env:SOTC_MUTE = '1'
$env:SOTC_SHADER_PREP = if ($Prep) { $Prep } else { $null }
$env:SOTC_PRELOAD_THREADS = if ($Threads -ne '') { $Threads } else { $null }
$exe = Join-Path $PSScriptRoot "runtime\Build\x64-Clang-$Build\shadps4.exe"
$full = if ($Fullscreen) { 'true' } else { 'false' }
$p = Start-Process $exe -ArgumentList '-f', $full, '--game', '"D:/Games/CUSA08809T/eboot.bin"' `
    -WorkingDirectory $dir -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput $out -RedirectStandardError (Join-Path $dir 'stderr.txt')
$clock = [Diagnostics.Stopwatch]::StartNew()
$env:SOTC_SHADER_PREP = $null; $env:SOTC_PRELOAD_THREADS = $null
$first = $null
$stayUntil = $null
while ($clock.Elapsed.TotalSeconds -lt $Max) {
    Start-Sleep -Milliseconds 100
    if ($p.HasExited) { "exited after {0:n1}s" -f $clock.Elapsed.TotalSeconds; break }
    $t = $clock.Elapsed.TotalSeconds
    if ($Shot -and $t -ge $ShotAt) { & (Join-Path $PSScriptRoot 'screencap.ps1') -Out $Shot | Out-Null; "shot at {0:n1}s" -f $t; $Shot = '' }
    if ($Key -and $t -ge $KeyAt) {
        $p.Refresh(); [void][PK]::SetForegroundWindow($p.MainWindowHandle); Start-Sleep -Milliseconds 300
        $vk = [byte][System.Windows.Forms.Keys]$Key; $scan = [byte][PK]::MapVirtualKey($vk, 0)
        [PK]::keybd_event($vk, $scan, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 200
        [PK]::keybd_event($vk, $scan, 2, [UIntPtr]::Zero)
        "pressed $Key at {0:n1}s" -f $t; $Key = ''
    }
    if ($Shot2 -and $t -ge $Shot2At) { & (Join-Path $PSScriptRoot 'screencap.ps1') -Out $Shot2 | Out-Null; "shot at {0:n1}s" -f $t; $Shot2 = '' }
    if (!$first -and (Select-String -Path $out -Pattern 'first flip' -Quiet -ErrorAction SilentlyContinue)) {
        $first = $t; "first frame after {0:n1}s" -f $t
        if ($Stay -le 0) { break }
        $stayUntil = $t + $Stay
    }
    if ($stayUntil -and $t -ge $stayUntil) { break }
    if (Get-Process ShadowOfTheColossus -ErrorAction SilentlyContinue) { "ABORTED: the user's game started"; break }
}
"alive: $(-not $p.HasExited)"
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep 2
Select-String -Path $out -Pattern 'Preloaded|Cache pack|preparation skipped|Critical|stats/3s' -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Line.Substring(0, [Math]::Min(190, $_.Line.Length)) } | Select-Object -Last 8
