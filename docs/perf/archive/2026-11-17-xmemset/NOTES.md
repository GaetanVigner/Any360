# 2026-11-17: the XDK's vector fill (XMemSet) native

docs/NATIVE_FUNCTIONS.md, "XMemSet". Bound in Crackdown at 0x82742E80 (base guest).

- `city_cpu_profile.txt`: where the processor time went in Crackdown's city before this step (`tools/cpu_profile.ps1`,
  50 s): the library code outside the D3D runtime is under 1% of the process (XAudio 0.6%, the C runtime 0.3%, XAPI
  0.1%); XMemSet was the first XAPI function (0.05%).
- `xdk_xapi_bench.txt`: the off-game benchmark: XMemSet is memset (2.9 ns at 16 bytes, 9.9 ns at 1 KB), the model
  2.6 to 35 times slower; 0 B allocated.
- `in_game_verify_bench.txt`: `--any360_xdk_verify=true`: 8,180,925 calls compared with the function's own code, 0
  different, none declined. `--any360_xdk_bench=true`: 10,496,932 calls, 21.6 -> 12.7 ns a call (1.7x); most of them
  fill 1 to 7 bytes, then 1,440 bytes (1.23 M calls, 60.3 -> 23.5 ns). About 94 ms saved over the 150-second run.
