# Native functions: the rule

A native function is host code that does the work of a function titles link statically from the XDK's libraries
(the C runtime first, `layer/xdk`). It is Any360's, written once for every title: a title only says at which address
its build of the library has the function (`ANY360_XDK_BIND`, the address found by library recognition,
`tools/library_scan.py`), and keeps the function's recompiled code as the fallback.

**The titles are test benches, not targets.** Test Drive Unlimited, Crackdown and The Simpsons Game are where a
native function is checked and measured; none of them is what it is written for. So:

- nothing in `layer/xdk` knows a title: no address, no size, no behaviour that only one title's build has, unless
  the function's own code has it in every build read so far;
- a function is chosen because titles in general link it and call it often (`db/xdk_seed.json` lists them with
  their address in each title), not because one title needs it;
- a title's own copy of a native function (Crackdown and The Simpsons Game still carry theirs for `memcpy` and
  `memset`, see State) is replaced by the shared one, never kept beside it;
- an optimization is judged on every title measured, and one that gains on a title and loses on another is not
  kept.

Titles call the critical ones millions of times a minute, and a wrong one corrupts a title's memory silently. So
every native function, from the first line, follows the same five steps, in this order. A function that skips one is
not bound in any title.

| Step | What | Where | Passes when |
|---|---|---|---|
| 1 | a reference model | `layer/xdk/include/any360/xdk/<library>_reference.h` | it is the function's work written the plainest way, read from the function's own code (what it writes, what it returns, what it leaves on the stack) |
| 2 | unit tests against the model | `layer/xdk/tests/xdk_<library>_tests.cpp`, run by `layer/sys/test.ps1` | every accepted call leaves memory exactly as the model does, every declined call leaves it untouched, and a call is declined only for the reasons the header gives |
| 3 | a benchmark | `layer/xdk/tests/xdk_<library>_bench.cpp` | it prints nanoseconds a call, size by size, for the model, the version first written (kept in the benchmark as the fixed point) and the current one, then one figure for each measured title's mix |
| 4 | the check in game | `--any360_xdk_verify=true` | each title that binds it runs with every call done both ways and compared, and its log reports 0 different |
| 5 | the measure in game | `--any360_xdk_bench=true` | the log gives, size class by size class, the calls and the nanoseconds of the function's own code and of the native one, and the sizes the title passes most |

Then, and only then, optimization:

- A change to a native function is kept when the benchmark shows it faster on the measured titles' mixes (all of
  them: see above) **and** the unit tests still pass **and** the check in game still reports 0 different. The unit
  tests run before the benchmark is read.
- A change the benchmark does not confirm is taken out, however reasonable it looks. Measured numbers, not expected
  ones, go in commit messages and in this file.
- The mixes come from step 5, not from a guess: the sizes titles really pass decide which path is worth writing by
  hand. One title's mix is one sample: while a single title has been measured, what was tuned on it is to be
  measured again on the next title before it counts as settled.
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

1. Find the function in the titles (`tools/library_scan.py`, the call counts of a trace) and read its generated
   code in each build at hand: every store and every exit, the registers it returns. The native function does what
   all of them do; where builds differ, it declines the case or the difference is a parameter of the handler.
2. Write the model, then the tests (exhaustive where sizes and alignments are small, random above, the declined
   cases, the physical view), then the function in the plainest way that passes.
3. Add it to the benchmark with its `first` version, to `xdk_natives.h` (a handler, a `Function` value: the verify
   and bench modes cover it from there) and to `layer/sys/native.json`.
4. Bind it in a title (`ANY360_XDK_BIND(address, handler)`), run the title with `--any360_xdk_verify=true` until
   the function has been called in every part of the game that can be reached, then with `--any360_xdk_bench=true`.
   Then the next title: the function is done when every title at hand binds the shared one.
5. Put each title's mix in the benchmark, optimize under the rule above, and record the numbers below.

Each title's library build differs (no function is byte-identical across the three titles so far): the address is
per title, the native function is shared, and the check in game is run again for every title that binds it.

## State

Three functions of the C runtime, the same code in the three titles at hand. Unit tests: `xdk_crt_tests`,
1,090,912 checks.

| Function | Handler | Declines | Test Drive Unlimited | Crackdown | The Simpsons Game |
|---|---|---|---|---|---|
| `memcpy` | `CrtMemcpy` | destination inside the source, ranges that are not plain memory | bound | bound (both of its guests) | bound |
| `memcpy`'s plain forward copy | `CrtCopyForward` | the same | bound | not found in its code yet | bound |
| `memset` | `CrtMemset` | ranges that are not plain memory | bound | bound (both of its guests) | bound |

Crackdown and The Simpsons Game had their own copies of these; they are gone from Crackdown, and The Simpsons Game
keeps its own only for a build without Any360 (it can still be built without). The Simpsons Game's copy of `memcpy`
saved the destination's address and not its whole register, the mistake the check in game found here.

What those titles' own code still replaces and Any360 does not have yet, the next functions under this rule:
`memmove` (Crackdown), a copy of whole 64-byte blocks and the XDK's copy for write-combined memory (The Simpsons
Game).

### The check in game

The code as it is, every call done both ways and compared (`--any360_xdk_verify=true`), a gamepad connected and the
title played past its menus, 2026-11-10:

| Title | Calls compared | `memcpy` | forward copy | `memset` | Different | Reached |
|---|---|---|---|---|---|---|
| Test Drive Unlimited | 14 M | 12.7 M | 72 k | 1.25 M | 0 | past the title screen, scenes of 6,000 draws a frame |
| Crackdown | 21 M | 8.9 M | not bound | 12.1 M | 0 | the city |
| The Simpsons Game | 53 M | 44.1 M | 1.5 M | 7.3 M | 0 | a level |

What it found that the unit tests had not: `memcpy` saves the 64 bits of its destination register below the stack
pointer, and Test Drive Unlimited calls it with a register whose high half is not zero (13,856 calls in 13.7 M).
The model had the high half as zero, so the tests passed on a wrong model. The model, the function and a unit test
(`SavedRegister`) now have it.

### The measure in game

The function's own recompiled code against the native one, on every call (`--any360_xdk_bench=true`):

| Title | `memcpy` | forward copy | `memset` |
|---|---|---|---|
| Test Drive Unlimited | 70.0 -> 13.1 ns a call (5.3x), 13.2 M calls | 686 -> 32.7 ns (21x), 73 k calls | 59.2 -> 21.3 ns (2.8x), 713 k calls |
| Crackdown | 41.5 -> 14.7 ns (2.8x), 29.7 M calls | | 30.4 -> 13.9 ns (2.2x), 38.3 M calls |
| The Simpsons Game | 22.7 -> 9.8 ns (2.3x), 43.2 M calls | 79.3 -> 14.7 ns (5.4x), 2.05 M calls | 55.9 -> 11.3 ns (4.9x), 8.7 M calls |

What these figures say and do not say:

- They carry the cost of reading the clock and of memory that is not in the cache (a call of 0 bytes reads 9 to
  22 ns): they compare the function's own code with the native one, and cannot tell two native versions apart. The
  benchmark does that.
- Over 100 to 150 seconds of play, the natives save about 0.8 second of processor time in Test Drive Unlimited,
  1.4 in Crackdown and 1.1 in The Simpsons Game: around 1% of one thread. No gain in frames was measured.
- For Crackdown and The Simpsons Game the comparison is with the recompiled code, not with what they ran before:
  their own copies already used the host's `memmove` and `memset`. What they gain is the tested, shared function
  and the paths for small sizes.

### The titles' sizes and what optimization kept

| Title | Copies | Fills |
|---|---|---|
| Test Drive Unlimited | 16 bytes: 50%, 64: 32%, 48: 9% | 120 bytes: 65%, 64: 12%, 128: 5% |
| Crackdown | 104 bytes: 28%, 12: 14%, 64: 10%, 40: 7%; a third between 65 and 128 | 16 bytes: 43%, 48: 35% |
| The Simpsons Game | 64 bytes: 50%, 12: 39%, 192: 3% | 64 bytes: 59%, 1 to 2 KB: 18%, 48: 8% |

`xdk_crt_bench`, each title's mix with its sizes in random order, the version first written (the host's `memmove`
and `memset` behind the same checks) against the code as it is:

| | Test Drive Unlimited | Crackdown | The Simpsons Game | The three |
|---|---|---|---|---|
| `memcpy` | 6.5 -> 5.8 ns a call (1.12x) | 9.6 -> 7.5 ns (1.27x) | 6.5 -> 5.6 ns (1.16x) | 1.2x |
| `memset` | 10.2 -> 7.6 ns (1.34x) | 6.6 -> 4.3 ns (1.53x) | 7.9 -> 6.8 ns (1.17x) | 1.3x |

- Kept: for `memcpy`, sizes up to 128 bytes as overlapping loads and stores; for `memset`, sizes up to 128 bytes
  as overlapping stores. No title is slower than with the first version.
- Taken out: one path with no branch on the size for 16 to 64 bytes (0.99x for `memcpy` on Test Drive Unlimited's
  mix, no better than the branches for `memset`).
- What one title would have got wrong: on Test Drive Unlimited's mix alone, the part of the `memcpy` path from 65
  to 128 bytes gained nothing (1.13x with it, 1.15x without) and was taken out. Crackdown, a third of whose copies
  are that size, gains from it (1.31x with it, 1.23x without) and neither of the others loses: it is back. An
  optimization is settled only when every title at hand has been measured.

### Found on the way

- Crackdown crashed after 40 to 55 seconds with a gamepad connected and the input group on `any360::sys`: its
  host code (the button-glyph overlay) and Any360's input sampling thread both called the runtime's input system,
  which takes one caller at a time. `any360::host::rexglue::LockHostInput` is the lock a title's host code takes
  around such a call; the overlay takes it.
- Crackdown's title screen, left without a frame limit (more than 1,500 frames a second), does not take the Start
  button; at 60 frames a second it does. Not looked into: it is the title's port, not a native function.
