# 2026-11-16: libpng 1.0.5's png_read_filter_row native

docs/NATIVE_FUNCTIONS.md, "libpng's png_read_filter_row". Bound in Crackdown at 0x82A7F5D0 (the PNG loader's copy).

- `xdk_libpng_bench.txt`: the off-game benchmark, by filter and pixel size, on smooth rows. Paeth, a pixel at a
  time in SSE2: 3.4x (24-bit pixels) and 5.0x (32-bit) over the version first written; the other filters as first
  written (Up 0.02 ns a byte; Sub and Average 0.4 to 1.7 ns a byte, a chain from byte to byte). A Paeth without
  branches, one byte at a time, measured 0.93x on 8-bit pixels: not kept. 0 B allocated a call.
- `in_game_verify_bench.txt`: `--any360_xdk_verify=true`: 12,452 calls compared with the function's own code, 0
  different, none declined. `--any360_xdk_bench=true`: 11,729 -> 1,156 ns a call (10.1x); rows of 3,840 bytes the
  most frequent (4,320 calls). About 11,500 calls come before the city shows, about 950 in its first seconds, none
  after: about 10 ms saved at the city's entry, spread over its first frames.
- `png_ab10.txt`, `png_frame_lows.txt`: A/B of 5 launches a variant, alternating, the city route, cores 0-7,
  NVIDIA; before = the same build without this binding (inflate_fast bound in both).

| | before | after |
|---|---|---|
| steady FPS mean (median) | 281.6 (294.1) | 301.9 (318.2), z 1.28 |
| steady 10% low | 233.4 | 256.0, z 1.81 |
| steady 1% low | 188.2 | 206.7, z 1.63 |
| dip 1% low | 129.4 | 151.8, z 0.52 |
| dip slowest frame | 15.4 ms | 16.4 ms |
| private memory mean | 2,915 MB | 2,886 MB |

Reading: within noise. The steady periods cannot move with this function (no call after the city's first seconds),
and this series ran slower than the others in both variants (launches down to 170 FPS: the machine was busier). The
dip shows nothing either way at 5 launches. One "before" launch reported its window not answering for 5 s at start,
while 1,152 pipelines loaded from storage (4.1 s): before any call of the function, not its doing.
