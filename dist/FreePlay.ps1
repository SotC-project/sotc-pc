# Shadow of the Colossus - free play: start directly in a battle with any colossus.
# Uses the engine's own debug start options; your normal save is not loaded or changed.
# Run FreePlay.bat for the window, or e.g.:
#   pwsh -File FreePlay.ps1 -Colossus 3 -AllItems on -UnlimitedGrip on
#   pwsh -File FreePlay.ps1 -Colossus 1 -Difficulty hard -Challenge on
param(
    [ValidateRange(0, 16)] [int]$Colossus = 0,
    [ValidateSet('easy', 'normal', 'hard', '')] [string]$Difficulty = '',
    [ValidateSet('on', 'off', '')] [string]$Challenge = '',
    [ValidateSet('on', 'off', '')] [string]$Invincible = '',
    [ValidateSet('on', 'off', '')] [string]$UnlimitedGrip = '',
    [ValidateSet('on', 'off', '')] [string]$MaxDamage = '',
    [ValidateSet('on', 'off', '')] [string]$AllItems = '',
    [string]$GameDir = 'D:\Games\CUSA08809'
)

$names = @('Valus (Minotaur)', 'Quadratus (Mammoth)', 'Gaius (Knight)', 'Phaedra (Horse)',
    'Avion (Bird)', 'Barba (Minotaur B)', 'Hydrus (Eel)', 'Kuromori (Gecko)',
    'Basaran (Tortoise)', 'Dirge (Sand serpent)', 'Celosia (Lion)', 'Pelagia (Lake beast)',
    'Phalanx (Flying serpent)', 'Cenobia (Cerberus)', 'Argus (Minotaur C)', 'Malus (Wizard)')

# Time attack limits of the PS4 game in seconds, per colossus: normal, hard. Easy has no time
# attack in the game and uses the normal limits here. Colossi 12-15 are estimates (the published
# tables found list the other twelve); edit them here if you know the exact values.
$limits = @(
    @(150, 165), @(210, 270), @(360, 390), @(300, 375), @(300, 255), @(210, 360), @(540, 450), @(360, 420),
    @(330, 345), @(330, 270), @(270, 255), @(360, 420), @(360, 330), @(270, 300), @(390, 510), @(570, 645))

if ($Colossus -eq 0) {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    [System.Windows.Forms.Application]::EnableVisualStyles()
    $form = New-Object System.Windows.Forms.Form
    $form.Text = 'Shadow of the Colossus - Free play'
    $form.StartPosition = 'CenterScreen'
    $form.FormBorderStyle = 'FixedDialog'
    $form.MaximizeBox = $false
    $form.ClientSize = New-Object System.Drawing.Size(420, 372)
    $form.Font = New-Object System.Drawing.Font('Segoe UI', 10)

    $y = 16
    function Add-Label($text) {
        $label = New-Object System.Windows.Forms.Label
        $label.Text = $text
        $label.Location = New-Object System.Drawing.Point(18, ($script:y + 3))
        $label.AutoSize = $true
        $form.Controls.Add($label)
    }
    function Add-Combo($items, $selected) {
        $combo = New-Object System.Windows.Forms.ComboBox
        $combo.DropDownStyle = 'DropDownList'
        $combo.Location = New-Object System.Drawing.Point(120, $script:y)
        $combo.Width = 280
        [void]$combo.Items.AddRange($items)
        $combo.SelectedIndex = $selected
        $form.Controls.Add($combo)
        $script:y += 38
        return $combo
    }
    function Add-Check($text, $checked) {
        $check = New-Object System.Windows.Forms.CheckBox
        $check.Text = $text
        $check.Checked = $checked
        $check.Location = New-Object System.Drawing.Point(20, $script:y)
        $check.AutoSize = $true
        $form.Controls.Add($check)
        $script:y += 30
        return $check
    }

    Add-Label 'Colossus'
    $colossusBox = Add-Combo (0..15 | ForEach-Object { '{0}. {1}' -f ($_ + 1), $names[$_] }) 0
    Add-Label 'Difficulty'
    $difficultyBox = Add-Combo @('Easy', 'Normal', 'Hard') 1
    $challengeBox = Add-Check 'Challenge: timed run, graded D to S+ (F if Wander dies)' $false
    $limitLabel = New-Object System.Windows.Forms.Label
    $limitLabel.Location = New-Object System.Drawing.Point(38, $y)
    $limitLabel.AutoSize = $true
    $limitLabel.ForeColor = [System.Drawing.Color]::DimGray
    $form.Controls.Add($limitLabel)
    $y += 30
    $itemsBox = Add-Check 'All weapons and items (time attack rewards)' $true
    $invincibleBox = Add-Check 'Take no damage' $false
    $gripBox = Add-Check 'Unlimited grip (stamina)' $false
    $damageBox = Add-Check 'Maximum attack damage' $false

    $refresh = {
        $pair = $limits[$colossusBox.SelectedIndex]
        $seconds = if ($difficultyBox.SelectedIndex -eq 2) { $pair[1] } else { $pair[0] }
        $note = if ($colossusBox.SelectedIndex -ge 11 -and $colossusBox.SelectedIndex -le 14) { ' (estimate)' } else { '' }
        $limitLabel.Text = 'Time to beat: {0}:{1:00}{2}' -f [math]::Floor($seconds / 60), ($seconds % 60), $note
        # A graded run is played without help.
        foreach ($box in @($itemsBox, $invincibleBox, $gripBox, $damageBox)) { $box.Enabled = -not $challengeBox.Checked }
    }
    $colossusBox.Add_SelectedIndexChanged($refresh)
    $difficultyBox.Add_SelectedIndexChanged($refresh)
    $challengeBox.Add_CheckedChanged($refresh)
    & $refresh

    $start = New-Object System.Windows.Forms.Button
    $start.Text = 'Start battle'
    $start.Size = New-Object System.Drawing.Size(130, 34)
    $start.Location = New-Object System.Drawing.Point(140, ($y + 10))
    $start.DialogResult = 'OK'
    $form.AcceptButton = $start
    $form.Controls.Add($start)
    $cancel = New-Object System.Windows.Forms.Button
    $cancel.Text = 'Cancel'
    $cancel.Size = New-Object System.Drawing.Size(110, 34)
    $cancel.Location = New-Object System.Drawing.Point(290, ($y + 10))
    $cancel.DialogResult = 'Cancel'
    $form.CancelButton = $cancel
    $form.Controls.Add($cancel)

    if ($form.ShowDialog() -ne 'OK') { exit 0 }
    $Colossus = $colossusBox.SelectedIndex + 1
    $Difficulty = @('easy', 'normal', 'hard')[$difficultyBox.SelectedIndex]
    $Challenge = if ($challengeBox.Checked) { 'on' } else { 'off' }
    $AllItems = if ($itemsBox.Checked) { 'on' } else { 'off' }
    $Invincible = if ($invincibleBox.Checked) { 'on' } else { 'off' }
    $UnlimitedGrip = if ($gripBox.Checked) { 'on' } else { 'off' }
    $MaxDamage = if ($damageBox.Checked) { 'on' } else { 'off' }
}
if (-not $Difficulty) { $Difficulty = 'normal' }
if ($Challenge -eq 'on') {
    $Invincible = 'off'; $UnlimitedGrip = 'off'; $MaxDamage = 'off'; $AllItems = 'off'
}

$overlay = Join-Path "$GameDir-mods" 'CommandLineArgs.txt'
if (-not (Test-Path $overlay)) {
    Write-Host "Launch options not found ($overlay). In the game choose Settings > Graphics > Apply and Restart once first."
    exit 1
}
if (Get-Process shadps4, ShadowOfTheColossus -ErrorAction SilentlyContinue) {
    Write-Host "The game is already running. Close it first."
    exit 1
}
$exe = Join-Path $PSScriptRoot 'ShadowOfTheColossus.exe'
if (-not (Test-Path $exe)) { $exe = Join-Path $PSScriptRoot '..\runtime\Build\x64-Clang-Release\shadps4.exe' }
if (-not (Test-Path $exe)) { $exe = Join-Path $PSScriptRoot '..\runtime\Build\x64-Clang-RelWithDebInfo\shadps4.exe' }

$lines = @(Get-Content $overlay)
$lines += '+nointro', '+skipSplash=true', "+boss=$($Colossus - 1)"
if ($Difficulty -eq 'hard') { $lines += '+shadow_setHardMode=true' }
if ($Difficulty -eq 'easy') { $lines += '+shadow_setEasyMode=true' }
if ($Invincible -eq 'on') { $lines += '+shadow_noDamage=true' }
if ($UnlimitedGrip -eq 'on') { $lines += '+shadow_unlimitedGrip=true' }
if ($MaxDamage -eq 'on') { $lines += '+shadow_maxAttackDamage=true' }
# The game's one option for all items gives nothing; its item sets do.
if ($AllItems -eq 'on') {
    foreach ($set in 'WhistleArrow', 'ExplosionArrow', 'Harpoon', 'SwordOfDormin', 'SwordOfLife', 'AncientBow', 'Parachute') {
        $lines += "+shadow_startWith${set}Set=true"
    }
}

$backup = "$overlay.freeplay"
Copy-Item $overlay $backup -Force
try {
    Set-Content $overlay $lines -Encoding ascii
    Write-Host ""
    Write-Host ("Starting battle {0}: {1} ({2})" -f $Colossus, $names[$Colossus - 1], $Difficulty)
    if ($Challenge -eq 'on') {
        $pair = $limits[$Colossus - 1]
        $seconds = if ($Difficulty -eq 'hard') { $pair[1] } else { $pair[0] }
        # The runtime reads this: colossus, difficulty, time limit in seconds.
        $env:SOTC_CHALLENGE = "$Colossus,$Difficulty,$seconds"
        Write-Host ("Challenge: time to beat {0}:{1:00}" -f [math]::Floor($seconds / 60), ($seconds % 60))
    }
    Push-Location $PSScriptRoot
    Start-Process -FilePath $exe -ArgumentList '--game', "`"$GameDir\eboot.bin`"" -Wait
    Pop-Location
} finally {
    $env:SOTC_CHALLENGE = $null
    # Put the normal launch options back whatever happened.
    Move-Item $backup $overlay -Force
}
