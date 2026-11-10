# Native functions: the rule

A native function is host code that does the work of a function of the title: a library function the title links
statically (the XDK's C runtime first, `layer/xdk`), bound at the address library recognition finds it at
(`tools/library_scan.py`), its recompiled code kept as the fallback. A title calls the critical ones millions of times
a minute, and a wrong one corrupts the title's memory silently. So every native function, from the first line, follows
the same five steps, in this order. A function that skips one is not bound in a title.

| Step | What | Where | Passes when |
|---|---|---|---|
| 1 | a reference model | `layer/xdk/include/any360/xdk/<library>_reference.h` | it is the function's work written the plainest way, read from the function's own code (what it writes, what it returns, what it leaves on the stack) |
| 2 | unit tests against the model | `layer/xdk/tests/xdk_<library>_tests.cpp`, run by `layer/sys/test.ps1` | every accepted call leaves memory exactly as the model does, every declined call leaves it untouched, and a call is declined only for the reasons the header gives |
| 3 | a benchmark | `layer/xdk/tests/xdk_<library>_bench.cpp` | it prints nanoseconds a call, size by size, for the model, the version first written (kept in the benchmark as the fixed point) and the current one, then one figure for a title's measured mix |
| 4 | the check in game | `--any360_xdk_verify=true` | the title runs with every call done both ways and compared, and the log reports 0 different |
| 5 | the measure in game | `--any360_xdk_bench=true` | the log gives, size class by size class, the calls and the nanoseconds of the function's own code and of the native one |

Then, and only then, optimization:

- A change to a native function is kept when the benchmark shows it faster on the title's mix **and** the unit tests
  still pass **and** the check in game still reports 0 different. The unit tests run before the benchmark is read.
- A change the benchmark does not confirm is taken out, however reasonable it looks. Measured numbers, not expected
  ones, go in commit messages and in this file.
- The title's mix comes from step 5, not from a guess: the sizes a title really passes decide which path is worth
  writing by hand.
- The benchmark's `first` version is never edited: it is what later versions are measured against.
- A difference the check in game finds is first written as a unit test that fails (the model was wrong, or the
  tests did not reach the case), then fixed. The model is only as good as the reading of the function's code: the
  check in game is what ties it to the title.

## What a native function may and may not do

- It does the common case and declines the rest by returning `false` **before writing anything**; the binding then
  runs the function's own code. What it declines is written in its header comment and tested (step 2 fails a
  function that declines a call its header says it takes: no passing by declining).
- It leaves everything the function's own code leaves that a caller could see: the bytes written, the value
  returned, and what the function stores on the stack (the CRT's `memcpy` saves its destination below the stack
  pointer; the native one writes the same 8 bytes).
- It is portable C++ with no dependency, header-only so that a title's binding inlines it. Nothing of a host runtime
  in `layer/xdk`; the host's part (reading the registers, the settings, the two in-game modes) is
  `layer/host/rexglue/include/any360/host/xdk_natives.h`.
- Guest memory is reached through `any360::xdk::Memory` only (the physical view's host offset), and only ranges that
  `IsPlainMemory` accepts: nothing that holds the GPU's registers, nothing that wraps or crosses into the physical
  view.

## Adding one

1. Find the function in the title (`tools/library_scan.py`, the call counts of a trace) and read its generated code:
   every store and every exit, the registers it returns.
2. Write the model, then the tests (exhaustive where sizes and alignments are small, random above, the declined
   cases, the physical view), then the function in the plainest way that passes.
3. Add it to the benchmark with its `first` version, to `xdk_natives.h` (a handler, a `Function` value: the verify
   and bench modes cover it from there) and to `layer/sys/native.json`.
4. Bind it in the title (`ANY360_XDK_BIND(address, handler)`), run the title with `--any360_xdk_verify=true` until
   the function has been called in every part of the game that can be reached, then with `--any360_xdk_bench=true`.
5. Put the title's mix in the benchmark, optimize under the rule above, and record the numbers below.

Each title's library build differs (no function is byte-identical across the three titles so far): the address is
per title, the native function is shared, and the check in game is run again for every title that binds it.

## State

Three functions of the C runtime, bound in Test Drive Unlimited (`src/xdk_natives.cpp` of its repository). Unit
tests: `xdk_crt_tests`, 1,090,912 checks.

| Function | Handler | Declines | Check in game | Own code, in game | Native, in game | Benchmark, title's mix |
|---|---|---|---|---|---|---|
| `memcpy` | `CrtMemcpy` | destination inside the source, ranges that are not plain memory | 1.86 M calls, 0 different | 70.0 ns a call | 13.1 ns (5.3x) | 6.8 -> 5.9 ns a call (1.15x the first version, 9x the model) |
| `memcpy`'s plain forward copy | `CrtCopyForward` | the same | 2 calls, 0 different | 686 ns | 32.7 ns (21x) | the same code as `memcpy` |
| `memset` | `CrtMemset` | ranges that are not plain memory | 1.14 M calls, 0 different | 59.2 ns | 21.3 ns (2.8x) | 10.6 -> 7.8 ns a call (1.36x the first version, 17x the model) |

Measured on Test Drive Unlimited over its first 100 seconds, on the development machine (2026-11-10). What the
figures say and do not say:

- The check in game of the code as it is ran with no gamepad connected, so the title stayed on its first screens:
  3.0 M calls, and only 2 of the forward copy. The version just before it (which also did copies of 65 to 128
  bytes itself; they are the host's `memmove` now, as in the first version) was checked with a gamepad, the title
  going on to scenes of several thousand draws a frame: 13.0 M calls (12.1 M `memcpy`, 69 k forward copies, 871 k
  `memset`), 0 different. The in-game times and the mix of sizes are from that longer run. To do again with a
  gamepad: `--any360_xdk_verify=true`.

- The title's mix (the longer run): half of its `memcpy` calls copy 16 bytes, a third 64 bytes, a tenth 48 bytes; two `memset` calls
  in three fill 120 bytes. Sizes above 128 bytes are under 3% of the calls of either.
- In game, over those 100 seconds, the three natives save about 0.8 second of processor time (13.2 M `memcpy` calls
  at 57 ns saved each, the rest small): under 1% of one thread. The title's frame rate is locked there, so no gain
  in frames was measured.
- The in-game figures carry the cost of reading the clock and of memory that is not in the cache (a call of 0 bytes
  reads 22 ns): they compare the function's own code with the native one, and cannot tell two native versions
  apart. The benchmark does that.
- What optimization kept, on the benchmark's mix: for `memcpy`, sizes up to 64 bytes as overlapping loads and
  stores (1.15x the host's `memmove` behind the same checks); for `memset`, sizes up to 128 bytes as overlapping
  stores (1.36x). What it took out: the same `memcpy` path carried on to 128 bytes (1.13x, no better than stopping
  at 64, and slower than `memmove` at 96 bytes), and one path with no branch on the size for 16 to 64 bytes (0.99x
  for `memcpy`, 1.33x for `memset`: no better than the branches).
- What the check in game found that the unit tests had not: `memcpy` saves the 64 bits of its destination register
  below the stack pointer, and the title calls it with a register whose high half is not zero (13,856 calls in
  13.7 M). The model had the high half as zero, so the tests passed on a wrong model. The model, the function and a
  unit test (`SavedRegister`) now have it; a difference found in game always ends as a unit test.

Not measured: Crackdown and The Simpsons Game (their C runtime is at other addresses and is not bound yet), and
Test Drive Unlimited past its title screen.
