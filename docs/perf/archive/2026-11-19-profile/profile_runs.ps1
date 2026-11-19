$b = 'F:\Crackdown\crackdown-amd-ab\build-ab'
$s = 'C:\Users\Gaetan\AppData\Local\Temp\claude\4b3779b33b6aeead\c1219ef0-d3b6-41af-9cc5-9e73d9171756\scratchpad'
$ab = 'F:\Crackdown\crackdown-any360-sys\tools\any360-ab.ps1'
$route = '--any360_input_script=12:start,16:a,20:a,24:a,28:a,32:a,40:a,50:a'
for ($k = 1; $k -le 2; $k++) {
  Remove-Item "$b\logs\profile.txt", "$b\logs\profile.csv" -ErrorAction SilentlyContinue
  & $ab -Variants @("--any360_profile_seconds=80 --any360_profile_after=60 $route") -Seconds 160 -Build $b -Affinity 0xFF00 -Device Radeon
  "--- profile run $k"
  if (Test-Path "$b\logs\profile.txt") {
    Move-Item "$b\logs\profile.txt" "$s\profile_$k.txt" -Force
    Move-Item "$b\logs\profile.csv" "$s\profile_$k.csv" -Force
    Get-Content "$s\profile_$k.txt" -TotalCount 30
  } else { 'no profile' }
}
