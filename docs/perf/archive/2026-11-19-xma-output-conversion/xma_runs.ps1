$b = 'F:\Crackdown\crackdown-amd-ab\build-ab'
$s = 'C:\Users\Gaetan\AppData\Local\Temp\claude\4b3779b33b6aeead\c1219ef0-d3b6-41af-9cc5-9e73d9171756\scratchpad'
$ab = 'F:\Crackdown\crackdown-any360-sys\tools\any360-ab.ps1'
$r = '--any360_input_script=12:start,16:a,20:a,24:a,28:a,32:a,40:a,50:a'
function Run($name, $verify, $scalar, $extra) {
  $env:ANY360_XMA_STATS = "$s\xma_stats_$name.txt"
  $env:ANY360_XMA_VERIFY = $verify
  $env:ANY360_XMA_SCALAR = $scalar
  & $ab -Variants @("$extra $r") -Seconds 150 -Build $b -Affinity 0xFF00 -Device Radeon | Select-Object -First 2
  "--- $name"
}
Remove-Item "$b\logs\profile.txt", "$b\logs\profile.csv" -ErrorAction SilentlyContinue
Run 'verify' '1' '0' '--any360_profile_seconds=80 --any360_profile_after=60'
if (Test-Path "$b\logs\profile.txt") { Move-Item "$b\logs\profile.txt" "$s\profile_xma_verify.txt" -Force; Move-Item "$b\logs\profile.csv" "$s\profile_xma_verify.csv" -Force }
Run 'scalar1' '0' '1' ''
Run 'vector1' '0' '0' ''
Run 'scalar2' '0' '1' ''
Run 'vector2' '0' '0' ''
Remove-Item Env:ANY360_XMA_STATS, Env:ANY360_XMA_VERIFY, Env:ANY360_XMA_SCALAR
'done'
