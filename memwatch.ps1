# Samples the memory of a running game or test instance every few seconds until it exits.
# Usage: pwsh -File memwatch.ps1 [-Name shadps4] [-Every 5] [-Max 300]
# Prints working set, private (committed) memory, and the process's dedicated and shared GPU memory.
param([string]$Name = 'shadps4', [int]$Every = 5, [int]$Max = 300)
$clock = [Diagnostics.Stopwatch]::StartNew()
while ($clock.Elapsed.TotalSeconds -lt 60 -and -not (Get-Process $Name -ErrorAction SilentlyContinue)) { Start-Sleep -Milliseconds 500 }
$peak = @{ ws = 0; priv = 0; ded = 0; shared = 0 }
while ($clock.Elapsed.TotalSeconds -lt $Max) {
    $p = Get-Process $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if (!$p) { break }
    $gpu = (Get-Counter "\GPU Process Memory(pid_$($p.Id)_*)\Dedicated Usage", "\GPU Process Memory(pid_$($p.Id)_*)\Shared Usage" -ErrorAction SilentlyContinue).CounterSamples
    $ded = ($gpu | Where-Object Path -match 'dedicated' | Measure-Object CookedValue -Sum).Sum / 1MB
    $shared = ($gpu | Where-Object Path -match 'shared' | Measure-Object CookedValue -Sum).Sum / 1MB
    $ws = $p.WorkingSet64 / 1MB; $priv = $p.PrivateMemorySize64 / 1MB
    "{0,4:n0}s  RAM in use {1,6:n0} MB  committed {2,6:n0} MB  GPU dedicated {3,6:n0} MB  GPU shared {4,5:n0} MB" -f $clock.Elapsed.TotalSeconds, $ws, $priv, $ded, $shared
    foreach ($k in 'ws', 'priv', 'ded', 'shared') { $v = @{ ws = $ws; priv = $priv; ded = $ded; shared = $shared }[$k]; if ($v -gt $peak[$k]) { $peak[$k] = $v } }
    Start-Sleep -Seconds $Every
}
"peak: RAM in use {0:n0} MB, committed {1:n0} MB, GPU dedicated {2:n0} MB, GPU shared {3:n0} MB" -f $peak.ws, $peak.priv, $peak.ded, $peak.shared
