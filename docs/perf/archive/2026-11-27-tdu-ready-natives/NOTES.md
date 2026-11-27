# Test Drive Unlimited's ready natives (2026-11-27)

The 15 library functions Any360 had native and Test Drive Unlimited did not bind (docs/ROADMAP.md, "Xbox platform
functions by title"), each checked against the title's own code and timed; bound when at least as fast.

- Build: TDURewrite on Any360 45ebd3a (+ `xdk_cases.h`), Release, Ryzen 7 7800X3D (all 16 logical processors: the runtime resets the launch mask), RTX 5070 Ti.
- `verify.txt`: one launch to driving (`--any360_gpu=true`, all eight `any360::sys` groups, the input route of
  TDURewrite's benchmark) with `--any360_xdk_verify=true`, all 15 bound: 107 million calls compared, 0 different.
  Called: XAudio mix ramp 269,904, GetLastError / SetLastError about 641,000 each, mbtowc 39, wcsncpy 2. Never called:
  wcscmp, _finite, _isnan, _sptype, _add_exp, the five XAudio fills.
- `bench.txt`: the same route with `--any360_xdk_bench=true` (both sides timed call by call, alternating).
- `selftest.txt`: `--any360_d3d_selftest=true` with the ten uncalled functions' cases (`xdk_cases.h`), 3,000 generated
  calls each on the title's recompiled function and on the replacement: 0 different, 0 B and 0 allocations a call on
  both sides.

| function | own code | replacement | | bound |
|---|---:|---:|---:|---|
| XAudio mix ramp | 194.5 ns | 106.3 ns | 1.8x | yes |
| wcsncpy | 1,434.7 ns | 104.8 ns | 13.7x | yes |
| XAudio fill 256 | 236.3 ns | 16.3 ns | 14.5x | yes |
| XAudio fill 512 | 476.3 ns | 32.5 ns | 14.6x | yes |
| XAudio fill 1024 | 985.0 ns | 62.5 ns | 15.8x | yes |
| XAudio fill 2048 | 1,923.8 ns | 121.3 ns | 15.9x | yes |
| XAudio fill 4096 | 3,787.6 ns | 241.3 ns | 15.7x | yes |
| wcscmp | 38.8 ns | 20.0 ns | 1.94x | yes |
| _isnan | 5.0 ns | 3.8 ns | 1.33x | yes |
| _sptype | 6.3 ns | 5.0 ns | 1.25x | yes |
| _add_exp | 7.5 ns | 8.8 ns | 0.86x | no |
| _finite | 2.5 ns | 3.8 ns | 0.67x | no |
| mbtowc | 14.1 ns | 17.5 ns | 0.8x | no |
| GetLastError | 10.2 ns | 9.9 ns | 1.0x | no |
| SetLastError | 10.2 ns | 10.4 ns | 0.98x | no |

The mix ramp was left unbound before on the expectation that the title's recompiled vmaddfp (a multiply then an add)
would differ in the last bit from the fused replacement; the check compares the destination buffer and every register
the function leaves, and found none different on 269,904 mixes.
