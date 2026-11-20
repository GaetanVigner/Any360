$b = 'F:\Crackdown\crackdown-amd-ab\build-ab'
$s = 'C:\Users\Gaetan\AppData\Local\Temp\claude\4b3779b33b6aeead\c1219ef0-d3b6-41af-9cc5-9e73d9171756\scratchpad'
$ab = 'F:\Crackdown\crackdown-any360-sys\tools\any360-ab.ps1'
$r = '--any360_input_script=12:start,16:a,20:a,24:a,28:a,32:a,40:a,50:a'
for ($i = 1; $i -le 5; $i++) {
  foreach ($v in 'simd', 'nosimd') {
    $env:ANY360_XMA_STATS = "$s\xma_simd_stats_${v}_$i.txt"
    Remove-Item $env:ANY360_XMA_STATS -ErrorAction SilentlyContinue
    if ($v -eq 'nosimd') { $env:ANY360_XMA_NO_SIMD = '1' } else { Remove-Item Env:ANY360_XMA_NO_SIMD -ErrorAction SilentlyContinue }
    & $ab -Variants @("$r") -Seconds 150 -Build $b -Affinity 0xFF00 -Device Radeon | Select-Object -First 2
    "--- $v $i"
  }
}
Remove-Item Env:ANY360_XMA_STATS, Env:ANY360_XMA_NO_SIMD -ErrorAction SilentlyContinue
'done'
