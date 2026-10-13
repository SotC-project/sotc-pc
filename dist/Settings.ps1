# Shadow of the Colossus - PC build settings.
# Display options edit .\user\config.json. Graphics options are engine launch options, written to
# an overlay next to the game data (<game folder>-mods\CommandLineArgs.txt); the dump is not touched.
# Run Settings.bat, or e.g.: pwsh -File Settings.ps1 -Fps 120 -VSync off -MotionBlur off
param(
    [ValidateSet('30', '60', '90', '120', '144', '165', '240', 'uncapped', '')] [string]$Fps = '',
    [ValidateSet('on', 'off', '')] [string]$VSync = '',
    [ValidateSet('on', 'off', '')] [string]$Fullscreen = '',
    [ValidateSet('720p', '1080p', '1440p', '4k', '')] [string]$Window = '',
    [ValidateSet('on', 'off', '')] [string]$Shadows = '',
    [ValidateSet('on', 'off', '')] [string]$MotionBlur = '',
    [ValidateSet('on', 'off', '')] [string]$Bloom = '',
    [ValidateSet('on', 'off', '')] [string]$DepthOfField = '',
    [ValidateSet('on', 'off', '')] [string]$Vignette = '',
    [ValidateSet('on', 'off', '')] [string]$Fur = '',
    [ValidateSet('on', 'off', '')] [string]$Fog = '',
    [ValidateSet('on', 'off', '')] [string]$MistParticles = '',
    [ValidateSet('1x', '2x', '4x', '8x', '16x', '')] [string]$TextureFiltering = '',
    [ValidateSet('full', 'reduced', '')] [string]$DrawDistance = '',
    [ValidateSet('low', 'normal', 'high', '')] [string]$TextureDetail = '',
    [ValidateSet('off', 'normal', 'strong', '')] [string]$Sharpening = '',
    [ValidateSet('on', 'off', '')] [string]$Upscaling = '',
    [ValidateSet('low', 'medium', 'high', 'ultra', '')] [string]$Preset = '',
    [ValidateSet('auto', 'keyboard', 'xbox', 'playstation', '')] [string]$Prompts = '',
    # Loudness of the achievement notification sound, percent. -1 keeps the current value.
    [ValidateRange(-1, 100)] [int]$AchievementVolume = -1,
    [ValidateSet('english', 'spanish', 'latin-spanish', 'french', 'german', 'italian', 'portuguese', 'brazilian', 'russian', 'japanese', '')] [string]$Language = '',
    [string]$GameDir = 'D:\Games\CUSA08809'
)

$path = Join-Path $PSScriptRoot 'user\config.json'
if (-not (Test-Path $path)) {
    Write-Host "No config yet. Start the game once with Play.bat, then run this again."
    exit 1
}
$config = Get-Content $path -Raw | ConvertFrom-Json

# Engine options kept between runs of this script.
$graphicsPath = Join-Path $PSScriptRoot 'user\sotc_graphics.json'
$graphics = [ordered]@{ Shadows = 'on'; MotionBlur = 'on'; Bloom = 'on'; DepthOfField = 'on'; Vignette = 'on'; Fur = 'on'; Fog = 'on'; MistParticles = 'off'; TextureFiltering = '4x'; DrawDistance = 'reduced'; TextureDetail = 'normal'; Sharpening = 'normal' }
$choices = @{ TextureFiltering = @('1x', '2x', '4x', '8x', '16x'); DrawDistance = @('full', 'reduced'); TextureDetail = @('low', 'normal', 'high'); Sharpening = @('off', 'normal', 'strong') }
# A preset only fills in options; any switch given explicitly still wins.
$presets = @{
    low    = @{ Shadows = 'off'; MotionBlur = 'off'; Bloom = 'on'; DepthOfField = 'off'; Vignette = 'on'; Fur = 'off'; Fog = 'on'; MistParticles = 'off'; TextureFiltering = '2x'; DrawDistance = 'reduced'; TextureDetail = 'low'; Sharpening = 'normal' }
    medium = @{ Shadows = 'on'; MotionBlur = 'off'; Bloom = 'on'; DepthOfField = 'on'; Vignette = 'on'; Fur = 'on'; Fog = 'on'; MistParticles = 'off'; TextureFiltering = '4x'; DrawDistance = 'reduced'; TextureDetail = 'normal'; Sharpening = 'normal' }
    high   = @{ Shadows = 'on'; MotionBlur = 'on'; Bloom = 'on'; DepthOfField = 'on'; Vignette = 'on'; Fur = 'on'; Fog = 'on'; MistParticles = 'off'; TextureFiltering = '8x'; DrawDistance = 'full'; TextureDetail = 'normal'; Sharpening = 'normal' }
    ultra  = @{ Shadows = 'on'; MotionBlur = 'on'; Bloom = 'on'; DepthOfField = 'on'; Vignette = 'on'; Fur = 'on'; Fog = 'on'; MistParticles = 'off'; TextureFiltering = '16x'; DrawDistance = 'full'; TextureDetail = 'high'; Sharpening = 'normal' }
}
if (Test-Path $graphicsPath) {
    $saved = Get-Content $graphicsPath -Raw | ConvertFrom-Json
    foreach ($key in @($graphics.Keys)) { if ($saved.$key) { $graphics[$key] = $saved.$key } }
}

function Ask($prompt, $choices, $current) {
    Write-Host ""
    Write-Host "$prompt  (current: $current)"
    for ($i = 0; $i -lt $choices.Count; $i++) { Write-Host ("  {0}) {1}" -f ($i + 1), $choices[$i]) }
    $answer = Read-Host "Choice, or Enter to keep"
    if ($answer -match '^\d+$' -and [int]$answer -ge 1 -and [int]$answer -le $choices.Count) {
        return $choices[[int]$answer - 1]
    }
    return ''
}

$requested = @{ Shadows = $Shadows; MotionBlur = $MotionBlur; Bloom = $Bloom; DepthOfField = $DepthOfField; Vignette = $Vignette; Fur = $Fur; Fog = $Fog; MistParticles = $MistParticles; TextureFiltering = $TextureFiltering; DrawDistance = $DrawDistance; TextureDetail = $TextureDetail; Sharpening = $Sharpening }
$interactive = -not ($Fps -or $VSync -or $Fullscreen -or $Window -or $Upscaling -or $Preset -or $Language -or $Prompts -or ($AchievementVolume -ge 0) -or ($requested.Values | Where-Object { $_ }))
# The game presents every second tick of the frame clock in 3D scenes, so the clock runs at
# twice the wanted frame rate. Menus present every tick.
$currentFps = if ($config.GPU.vblank_frequency -ge 1000) { 'uncapped' } else { [string]($config.GPU.vblank_frequency / 2) }
$currentVSync = if ($config.GPU.present_mode -eq 'Fifo') { 'on' } else { 'off' }
$currentFull = if ($config.GPU.full_screen) { 'on' } else { 'off' }
# Console language ids. This is the European release: it has no US English (id 1) text and
# falls back to German for it, so "english" is British English.
$languages = [ordered]@{ english = 18; spanish = 3; 'latin-spanish' = 20; french = 2; german = 4; italian = 5; portuguese = 7; brazilian = 17; russian = 8; japanese = 0 }
$currentLanguage = ($languages.GetEnumerator() | Where-Object { $_.Value -eq $config.General.console_language }).Key
if (-not $currentLanguage) { $currentLanguage = "id $($config.General.console_language)" }

# Button prompts: the runtime swaps the game's glyphs for the keyboard or Xbox set in this folder,
# following the device last used. force.txt pins one style.
$texturePack = Join-Path $PSScriptRoot 'user\sotc_textures'
$forceFile = Join-Path $texturePack 'force.txt'
$achievementVolumeFile = Join-Path $PSScriptRoot 'user\sotc_achievement_volume.txt'
$currentAchievementVolume = if (Test-Path $achievementVolumeFile) { (Get-Content $achievementVolumeFile -TotalCount 1).Trim() } else { '100' }
$currentPrompts = if (Test-Path $forceFile) { (Get-Content $forceFile -TotalCount 1).Trim() } else { 'auto' }

if ($interactive) {
    $Fps = Ask 'Frame rate cap' @('30', '60', '90', '120', '144', '165', '240', 'uncapped') $currentFps
    $VSync = Ask 'VSync' @('on', 'off') $currentVSync
    $Fullscreen = Ask 'Fullscreen' @('on', 'off') $currentFull
    $Window = Ask 'Window size' @('720p', '1080p', '1440p', '4k') "$($config.GPU.window_width)x$($config.GPU.window_height)"
    $Upscaling = Ask 'FSR upscaling to the window size' @('on', 'off') $(if ($config.GPU.fsr_enabled) { 'on' } else { 'off' })
    $Language = Ask 'Language' @($languages.Keys) $currentLanguage
    $Prompts = Ask 'Button prompts' @('auto', 'keyboard', 'xbox', 'playstation') $currentPrompts
    $pick = Ask 'Achievement sound volume (%)' @('0', '25', '50', '75', '100') $currentAchievementVolume
    if ($pick) { $AchievementVolume = [int]$pick }
    $Preset = Ask 'Graphics preset (Enter to set options one by one)' @('low', 'medium', 'high', 'ultra') 'custom'
    if (-not $Preset) {
        foreach ($key in @($graphics.Keys)) {
            $options = if ($choices[$key]) { $choices[$key] } else { @('on', 'off') }
            $requested[$key] = Ask $key $options $graphics[$key]
        }
    }
}
if ($Preset) {
    foreach ($key in @($graphics.Keys)) { if (-not $requested[$key]) { $requested[$key] = $presets[$Preset][$key] } }
}
if ($Upscaling) { $config.GPU.fsr_enabled = ($Upscaling -eq 'on') }
if ($Language) { $config.General.console_language = $languages[$Language] }
if ($AchievementVolume -ge 0) {
    Set-Content $achievementVolumeFile $AchievementVolume -Encoding ascii
}
if ($Prompts) {
    foreach ($style in 'keyboard', 'xbox') {
        New-Item -ItemType Directory -Force (Join-Path $texturePack $style) | Out-Null
        Copy-Item (Join-Path $PSScriptRoot "glyphs\$style\*.bin") (Join-Path $texturePack $style) -Force
    }
    if ($Prompts -eq 'auto') {
        if (Test-Path $forceFile) { Remove-Item $forceFile }
    } else {
        Set-Content $forceFile $Prompts -Encoding ascii
    }
}

if ($Fps) {
    $config.GPU.vblank_frequency = if ($Fps -eq 'uncapped') { 1000 } else { [int]$Fps * 2 }
}
if ($VSync) {
    # Fifo waits for the display; Immediate presents as soon as a frame is ready.
    $config.GPU.present_mode = if ($VSync -eq 'on') { 'Fifo' } else { 'Immediate' }
}
if ($Fullscreen) {
    $config.GPU.full_screen = ($Fullscreen -eq 'on')
    $config.GPU.full_screen_mode = if ($Fullscreen -eq 'on') { 'Fullscreen (Borderless)' } else { 'Windowed' }
}
if ($Window) {
    $size = @{ '720p' = 1280, 720; '1080p' = 1920, 1080; '1440p' = 2560, 1440; '4k' = 3840, 2160 }[$Window]
    $config.GPU.window_width = $size[0]
    $config.GPU.window_height = $size[1]
}
$config | ConvertTo-Json -Depth 10 | Set-Content $path -Encoding utf8

foreach ($key in @($graphics.Keys)) { if ($requested[$key]) { $graphics[$key] = $requested[$key] } }
$graphics | ConvertTo-Json | Set-Content $graphicsPath -Encoding utf8

# Launch options: the game's own file plus ours. The engine reads "+name=value" lines.
$baseArgs = Join-Path $GameDir 'CommandLineArgs.txt'
if (Test-Path $baseArgs) {
    $flag = { param($value) if ($value -eq 'on') { 'true' } else { 'false' } }
    $lines = @((Get-Content $baseArgs) | Where-Object { $_.Trim() })
    # Per-mesh GPU triangle culling costs more than it saves on a PC GPU.
    $lines += '+doIndexBufferCulling=false', '+doSunShadowIndexBufferCulling=false'
    # Seconds the title menu waits before the opening film starts by itself.
    $lines += '+attractMovieTime=90'
    # The game then steps by time passed, not by frame: without it the game runs faster than it
    # should whenever the frame rate is above 60.
    $lines += '+TargetFramesPerSeconds=Unlocked'
    $lines += "+r_enableCascadeShadows=$(& $flag $graphics.Shadows)", "+r_enableSpotShadows=$(& $flag $graphics.Shadows)"
    $lines += "+r_motionBlurEnable=$(& $flag $graphics.MotionBlur)"
    $lines += "+r_bloomEnable=$(& $flag $graphics.Bloom)"
    $lines += "+r_dofEnable=$(& $flag $graphics.DepthOfField)"
    $lines += "+r_vignetteEnable=$(& $flag $graphics.Vignette)"
    $lines += "+r_furEnable=$(& $flag $graphics.Fur)"
    # Atmospheric haze; off gives a clear, higher-contrast view of the distance.
    $lines += "+fog_enable=$(& $flag $graphics.Fog)"
    # Soft mist particles (dust pouring off a colossus, drifting motes). This build draws them far
    # too bright - white columns and glowing blobs - so they are off unless asked for.
    $lines += "+showParticleGroup_Fog=$(& $flag $graphics.MistParticles)"
    # Anisotropy levels are powers of two: 0 = 1x ... 4 = 16x.
    $aniso = @{ '1x' = 0; '2x' = 1; '4x' = 2; '8x' = 3; '16x' = 4 }[$graphics.TextureFiltering]
    $lines += "+progSamplerAnisoLevel=$aniso", "+progSamplerAnisoLevel_BasePS4=$aniso", "+linearSamplerAnisoLevel=$aniso"
    # Mip bias: negative picks sharper texture levels (can shimmer), positive softer ones; 'normal' leaves the game's own value.
    if ($graphics.TextureDetail -ne 'normal') {
        $lines += "+samplerLodBias=$(@{ low = 1; high = -1 }[$graphics.TextureDetail])"
    }
    # Sharpening filter of the game's temporal anti-aliasing; 'normal' leaves the game's own value.
    if ($graphics.Sharpening -ne 'normal') {
        $lines += "+r_aaSharpenStrengthStatic=$(@{ off = 0; strong = 2 }[$graphics.Sharpening])"
    }
    if ($graphics.DrawDistance -eq 'reduced') {
        # Fades small objects out between 150 and 300 units; roughly halves the draw count.
        $lines += '+r_ModelFadeOutOverrideEnable=true', '+r_ModelFadeOutStartOverride=150', '+r_ModelFadeOutEndOverride=300'
    }
    $overlay = "$GameDir-mods"
    New-Item -ItemType Directory -Force $overlay | Out-Null
    Set-Content (Join-Path $overlay 'CommandLineArgs.txt') $lines -Encoding ascii
} else {
    Write-Host "Game folder not found ($GameDir); graphics options not written."
}

Write-Host ""
Write-Host ("Saved: frame clock {0} Hz, present mode {1}, fullscreen {2}, window {3}x{4}" -f `
        $config.GPU.vblank_frequency, $config.GPU.present_mode, $config.GPU.full_screen, `
        $config.GPU.window_width, $config.GPU.window_height)
Write-Host ("Button prompts: " + $(if (Test-Path $forceFile) { (Get-Content $forceFile -TotalCount 1).Trim() } else { 'auto (follows the device in use)' }))
Write-Host ("Graphics: " + (($graphics.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', '))
Write-Host ("Achievement sound volume: " + $(if (Test-Path $achievementVolumeFile) { (Get-Content $achievementVolumeFile -TotalCount 1).Trim() } else { '100' }) + '%')
