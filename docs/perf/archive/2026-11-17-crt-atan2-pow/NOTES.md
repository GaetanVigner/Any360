# 2026-11-17: the C runtime's atan2 and pow native, and the host's C library beside them

docs/NATIVE_FUNCTIONS.md, "atan2 and pow". Bound in Crackdown (0x82AB64C0, 0x82AB57F8).

- `xdk_crt_math_bench.txt`: the off-game benchmark (made-up tables, a CRT's shape): atan2 about 19.6 ns a call,
  pow about 56 ns, each as fast as its model (the same operations in the same order). 0 B allocated.
- `in_game_verify_bench.txt`: both launches stayed on the title screen (Start not pressed); the title calls both
  there. `--any360_xdk_verify=true`: atan2 185,832 calls, pow 111,850, compared with the functions' own code (f1's
  64 bits and the caller's two parameter words), 0 different, none declined. `--any360_xdk_bench=true`: atan2 88.9
  -> 56.7 ns a call (1.6x), pow 703 -> 190 ns (3.7x: the original goes through seven calls of its own, each storing
  and loading the value on the stack).
- `libc_compare.cpp` / `libc_compare.txt`: the owner's question, "would the host's C library do better?". The
  natives on Crackdown's own tables (read from its decrypted image, kept out of the repository) against the host's
  UCRT, 1 M calls each:

  | Function | Any360 | UCRT | UCRT faster by | UCRT results not the title's | most ULPs apart |
  |---|---|---|---|---|---|
  | sin | 16.3 ns | 8.4 ns | 1.9x | 30.6% | 39 |
  | cos | 17.0 ns | 8.1 ns | 2.1x | 33.9% | 28 |
  | asin | 14.1 ns | 7.9 ns | 1.8x | 23.1% | 2 |
  | atan2 | 18.6 ns | 13.6 ns | 1.4x | 47.2% | 3 |
  | pow | 55.3 ns | 8.4 ns | 6.6x | 27.4% | 2 |

  Mostly 1 ULP apart; sin and cos further near multiples of pi, where the title's reduction is the less accurate.
  Not the default: those bits are not the title's. Kept as a per-title optimization flag to try later, function by
  function, with an A/B and a check that the game still plays the same (docs/NATIVE_FUNCTIONS.md).
