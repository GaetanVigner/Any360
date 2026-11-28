# Alternating A/B of the bench with env-var variants. Variants: "name=ENV1=1;ENV2=0" strings.
param([string]$Exe = 'F:\Crackdown\any360-bench-waits\out\bench\bench\any360_bench.exe', [string]$Dir,
      [string[]]$Variants, [int]$Runs = 5, [int]$Frames = 3000, [string]$Mask = '0x3', [string]$Device = 'Radeon',
      [string[]]$Extra = @())
New-Item -ItemType Directory -Force $Dir | Out-Null
$runsFile = Join-Path $Dir 'runs.txt'
for ($r = 1; $r -le $Runs; $r++) {
  foreach ($v in $Variants) {
    $name, $envs = $v -split '=', 2
    $saved = @{}
    foreach ($kv in ($envs -split ';')) { if ($kv) { $k, $val = $kv -split '=', 2; $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $val) } }
    $log = "$name-$r.log"
    for ($w = 0; $w -lt 450; $w++) {
      $busy = (Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor | Where-Object { $_.Name -in '14','15' } | Measure-Object PercentProcessorTime -Maximum).Maximum
      if ($busy -lt 15) { break }
      Start-Sleep 2
    }
    $load = (Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor | Where-Object { $_.Name -in '14','15','_Total' } | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.PercentProcessorTime)" }) -join ' '
    $args2 = @('--frames', $Frames, "--any360_cpu_affinity=$Mask", "--any360_gpu_device=$Device", '--report-seconds', '3') + $Extra
    $p = Start-Process -FilePath $Exe -ArgumentList $args2 -RedirectStandardOutput (Join-Path $Dir $log) -RedirectStandardError (Join-Path $Dir "$log.err") -PassThru -NoNewWindow
    try { $p.PriorityClass = 'AboveNormal' } catch {}
    if (-not $p.WaitForExit(150000)) { Stop-Process -Id $p.Id -Force; Add-Content (Join-Path $Dir 'hangs.txt') $log }
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    Add-Content $runsFile "=== $name ($log)"
    $sum = (Get-Content (Join-Path $Dir $log) | Select-String 'any360_bench: [\d.]+ FPS' | Select-Object -First 1).Line
    Add-Content (Join-Path $Dir 'summary.txt') "$name $r [load before: $load] $sum"
    Write-Output "$name $r [$load] $sum"
  }
}

