# 2026-11-17: the C runtime's sin, cos and asin native

docs/NATIVE_FUNCTIONS.md, "sin, cos and asin". Bound in Crackdown (0x82AB6010, 0x82AB60E0, 0x82AB66E8).

- `xdk_crt_math_bench.txt`: the off-game benchmark (made-up tables, a CRT's shape): about 17-18 ns a call each, as
  fast as the models: the same operations in the same order (for the same bits) is the same chain of dependent
  fused multiply-adds, which no rewriting may shorten. 0 B allocated.
- `in_game_verify_bench.txt`: `--any360_xdk_verify=true`: sin 3,255,681 calls, cos 3,188,805, asin 9,032,261
  compared with the functions' own code (f1's 64 bits and the words below r1), 0 different, none declined.
  `--any360_xdk_bench=true`: sin 33.7 -> 32.2 ns a call (1.05x), cos 33.6 -> 28.7 (1.2x), asin 23.1 -> 18.3 (1.3x),
  about 80 ms saved over the 150-second run (19 M calls), below what a frame-rate A/B shows.
