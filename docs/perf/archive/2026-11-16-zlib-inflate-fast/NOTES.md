# 2026-11-16: zlib 1.1.4's inflate_fast native

The first native function of zlib (docs/NATIVE_FUNCTIONS.md, "zlib's inflate_fast"), bound in Crackdown at 0x82A88378
(the PNG loader's copy). D3DX's copy (0x8281BE00) was bound for these runs too, but a run with it bound alone made 0
calls to it: it is not bound any more.

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

Only the dip's slowest frame is near significance (z 1.98, five figures read, 5 launches a side), and its 5 ms is
more than the native one can give: the calls are made before and as the city shows (16,500 before, about 1,500 in
its first seconds, none after), 1.4 to 1.8 us saved each, so 2 to 3 ms in all even in one frame. This "before" had a
frame over 16.7 ms in 1 launch of 5; the same code had one in 4 of 5 in 2026-11-16-hitches. Reading: the frame
effect is within noise, at most 2 to 3 ms expected at the city's entry; the measured gain is 1.4x a call. The other
half of the PNG stalls is libpng's row work (filters, transformations): the next candidate.
