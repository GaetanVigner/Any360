$b = 'F:\Crackdown\crackdown-amd-ab\build-ab'
$s = 'C:\Users\Gaetan\AppData\Local\Temp\claude\4b3779b33b6aeead\c1219ef0-d3b6-41af-9cc5-9e73d9171756\scratchpad'
$ab = 'F:\Crackdown\crackdown-any360-sys\tools\any360-ab.ps1'
$r = '--any360_input_script=12:start,16:a,20:a,24:a,28:a,32:a,40:a,50:a'
Remove-Item "$b\logs\profile.txt", "$b\logs\profile.csv" -ErrorAction SilentlyContinue
& $ab -Variants @("--any360_profile_seconds=80 --any360_profile_after=60 $r") -Seconds 160 -Build $b -Affinity 0xFF00 -Device Radeon | Out-Null
if (Test-Path "$b\logs\profile.txt") {
  Move-Item "$b\logs\profile.txt" "$s\profile_reverbmem_after.txt" -Force
  Move-Item "$b\logs\profile.csv" "$s\profile_reverbmem_after.csv" -Force
}
'profile done'
$v = @()
for ($i = 0; $i -lt 5; $i++) { $v += "@exe:crackdown_before.exe $r"; $v += "@exe:crackdown_after.exe $r" }
& $ab -Variants $v -Seconds 150 -Build $b -Affinity 0xFF00 -Device Radeon | Set-Content -Encoding utf8 "$s\ab_reverbmem2_runs.txt"
'done'
