# Shadow of the Colossus - PC build: puts the game's own program in this folder and adds
# shortcuts. Run Install.bat, or: pwsh -File Install.ps1 [-NoDesktop] [-NoStartMenu]
#  - copies the optimised build here as ShadowOfTheColossus.exe;
#  - writes launch.ini next to it (game path, and this folder for saves and settings), so a
#    double click on the program, or any shortcut, boots straight into the game;
#  - creates "Shadow of the Colossus" shortcuts on the desktop and in the Start menu;
#  - gives this folder the game's icon.
# Windows does not let a script pin to the taskbar: start the game, right-click its taskbar
# icon and choose "Pin to taskbar".
param(
    [string]$GameDir = 'D:\Games\CUSA08809',
    [switch]$NoDesktop,
    [switch]$NoStartMenu
)

$build = Join-Path $PSScriptRoot '..\runtime\Build\x64-Clang-Release\shadps4.exe'
if (-not (Test-Path $build)) {
    Write-Host "Optimised build not found: $build"
    Write-Host "Run ..\build-lto.bat first."
    exit 1
}
$eboot = Join-Path $GameDir 'eboot.bin'
if (-not (Test-Path $eboot)) {
    Write-Host "Game data not found: $eboot"
    exit 1
}
if (Get-Process ShadowOfTheColossus -ErrorAction SilentlyContinue) {
    Write-Host "The game is running. Close it first."
    exit 1
}

$exe = Join-Path $PSScriptRoot 'ShadowOfTheColossus.exe'
Copy-Item $build $exe -Force
Write-Host "Program: $exe"

# The key pictures in the game's hints are redrawn by this script when bindings change.
Copy-Item (Join-Path $PSScriptRoot '..\glyphs.py') (Join-Path $PSScriptRoot 'glyphs.py') -Force

# First line: the game. Second line: the folder holding "user" (saves, settings, caches).
$launch = Join-Path $PSScriptRoot 'launch.ini'
Set-Content $launch @(($eboot -replace '\\', '/'), ($PSScriptRoot -replace '\\', '/')) -Encoding ascii

$shell = New-Object -ComObject WScript.Shell
$targets = @()
if (-not $NoDesktop) { $targets += [Environment]::GetFolderPath('Desktop') }
if (-not $NoStartMenu) { $targets += [Environment]::GetFolderPath('Programs') }
foreach ($folder in $targets) {
    $path = Join-Path $folder 'Shadow of the Colossus.lnk'
    # Start from a clean shortcut so no stale icon reference survives.
    if (Test-Path $path) { Remove-Item $path -Force }
    $shortcut = $shell.CreateShortcut($path)
    $shortcut.TargetPath = $exe
    $shortcut.WorkingDirectory = $PSScriptRoot
    $shortcut.IconLocation = "$exe,0"
    $shortcut.Description = 'Shadow of the Colossus'
    $shortcut.Save()
    Write-Host "Shortcut: $path"
}

# Folder icon.
$icon = Join-Path $PSScriptRoot 'sotc_mask.ico'
if (Test-Path $icon) {
    $ini = Join-Path $PSScriptRoot 'desktop.ini'
    if (Test-Path $ini) { attrib -s -h $ini }
    Set-Content $ini @('[.ShellClassInfo]', 'IconResource=sotc_mask.ico,0') -Encoding unicode
    attrib +s +h $ini
    attrib +r $PSScriptRoot
    Write-Host "Folder icon set."
}
