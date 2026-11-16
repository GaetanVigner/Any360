# 2026-11-16: zlib 1.1.4's inflate_fast native

The first native function of zlib (docs/NATIVE_FUNCTIONS.md, "zlib's inflate_fast"), bound in Crackdown (0x82A88378,
the PNG loader's copy; 0x8281BE00, D3DX's).

- `xdk_zlib_bench.txt`: the off-game benchmark (32 KB a call, three mixes): 1.17 ns a byte written (mostly
  literals) to 0.41 (mostly matches), 1.2 to 2.7 times the model, 0 B allocated a call.
- `in_game_verify_bench.txt`: `--any360_xdk_verify=true` (18,047 calls compared with the function's own code, 0
  different, none declined) and `--any360_xdk_bench=true` (5,383 -> 3,963 ns a call, 1.4x; by input size).
- `zlib_ab10.txt`, `zlib_frame_lows.txt`: A/B of 5 launches a variant, alternating, the city route, cores 0-7,
  NVIDIA; before = the same build without the two bindings.

| | before | after |
|---|---|---|
| steady FPS mean (median) | 317.8 (325.9) | 321.4 (330.4), z 1.44 |
| steady 10% low | 275.6 | 282.9, z 1.37 |
| steady 1% low | 232.3 | 232.8, z 0.20 |
| dip 1% low | 176.0 | 207.1, z 0.94 |
| dip slowest frame | 14.6 ms | 9.4 ms, z 1.98 |
| dip frames over 16.7 ms | 0.2 | 0.0 |
| private memory mean | 2,912 MB | 2,910 MB |

Only the dip's slowest frame is near significance: the calls are made before and as the city shows (16,500 before,
1,500 in its first seconds, none after), so steady frames cannot move. The other half of the PNG stalls is
libpng's row work (filters, transformations): the next candidate.
