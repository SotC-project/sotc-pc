# Verifies the keyboard bindings with real key events: launches the game, presses the given keys
# at the given seconds, and relies on SOTC_SCRIPT for in-engine screenshots and exit.
# Usage: pwsh -File keytest.ps1 -Keys "62:Escape,68:Down,69:Down" -Script "66:shot,74:shot,80:quit"
param([string]$Keys = "62:Escape", [string]$Script = "66:shot,80:quit")

Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class K {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
}
'@

$env:SOTC_SCRIPT = $Script
$exe = Join-Path $PSScriptRoot 'runtime\Build\x64-Clang-RelWithDebInfo\shadps4.exe'
$p = Start-Process $exe -ArgumentList '-f', 'false', '--game', '"D:/Games/CUSA08809/eboot.bin"' `
    -WorkingDirectory (Join-Path $PSScriptRoot 'dist') -PassThru `
    -RedirectStandardOutput "$env:TEMP\sotc_o.txt" -RedirectStandardError "$env:TEMP\sotc_e.txt"
$env:SOTC_SCRIPT = $null
$clock = [Diagnostics.Stopwatch]::StartNew()

foreach ($step in $Keys.Split(',')) {
    $at, $name = $step.Split(':')
    $wait = [int]$at - $clock.Elapsed.TotalSeconds
    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
    if ($p.HasExited) { "exited early"; break }
    $p.Refresh()
    [void][K]::SetForegroundWindow($p.MainWindowHandle)
    Start-Sleep -Milliseconds 300
    $vk = [byte][System.Windows.Forms.Keys]$name
    $scan = [byte][K]::MapVirtualKey($vk, 0)
    [K]::keybd_event($vk, $scan, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 200
    [K]::keybd_event($vk, $scan, 2, [UIntPtr]::Zero)
    "{0}s pressed {1}, game in foreground: {2}" -f $at, $name, ([K]::GetForegroundWindow() -eq $p.MainWindowHandle)
}
[void]$p.WaitForExit(60000)
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; "killed" }
Get-ChildItem (Join-Path $PSScriptRoot 'dist\user\screenshots\*.png') | Sort-Object LastWriteTime |
    Select-Object -Last 3 | ForEach-Object { "$($_.Name) $($_.Length)" }
