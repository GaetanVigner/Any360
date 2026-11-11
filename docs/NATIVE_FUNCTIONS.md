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
- a title's own copy of a native function is replaced by the shared one, never kept beside it; and a function
  that is the title's own code, in no library and in no other title (The Simpsons Game's copy of whole 64-byte
  blocks), is not a native function of Any360: it stays in the title;
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
| 4 | the check in game | `--any360_xdk_verify=true` | each title that binds it runs with every call done both ways and compared (the bytes written, the value returned, the stack slot of a leaf function, and the bytes next to the destination, which neither side should change), and its log reports 0 different |
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
- The benchmark's `first` version and the current one must differ by one thing at a time when a gain is given a
  cause: `memcpy`'s gain was first put down to its hand-written short copies, and measuring the host's `memmove`
  behind the same test of plain memory showed that test alone to be most of it.
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

Five functions that copy and fill, the same code in the three titles at hand, then ten string functions (below).
Unit tests: `xdk_crt_tests` (1,248,176 checks) and `xdk_xapi_tests` (1,772,293 checks).

| Function | Library | Handler | Declines | Test Drive Unlimited | Crackdown | The Simpsons Game |
|---|---|---|---|---|---|---|
| `memcpy` | CRT | `CrtMemcpy` | destination inside the source, ranges that are not plain memory | bound | bound (both of its guests) | bound |
| `memcpy`'s plain forward copy | CRT | `CrtCopyForward` | the same | bound | bound (base guest) | bound |
| `memset` | CRT | `CrtMemset` | ranges that are not plain memory | bound | bound (both guests) | bound |
| `memmove` | CRT | `CrtMemmove` | what `memcpy` declines on `memcpy`'s side, ranges that are not plain memory on the other | bound | bound (both guests) | bound |
| the vector copy (`XMemCpy`) | XAPI | `XMemCpy` | ranges that overlap, ranges that are not plain memory, sizes below 16 bytes when the title's byte step is not given or not 1 | bound | bound (base guest) | bound |

### The string functions (2026-11-11)

Ten functions of the C runtime that are the same instructions in the three titles, found by comparing every
function of the three (602 are identical), most call sites first. Each has its model and unit tests
(`xdk_crt_tests`, 1,248,176 checks with them); the first four have a table in `xdk_crt_bench`.

| Function | Handler | Declines | Test Drive Unlimited | Crackdown | The Simpsons Game |
|---|---|---|---|---|---|
| `strchr` | `CrtStrchr` | a string that reaches the GPU's register window | bound: 906,908 calls | bound: 18,831 | bound: 1,653 |
| `strrchr` | `CrtStrrchr` | the same | bound: 38 | bound: 1,225 | bound: 5 |
| `strncmp` | `CrtStrncmp` | the same | bound: 32,740 | bound: 21,021 | bound: 18 |
| `wcslen` | `CrtWcslen` | the same | bound: 253 | bound: 155,149 | not called: not bound |
| `tolower` | `CrtTolower` | nothing (the "C" locale's, on the register) | bound: 869 | bound: 88,097 | not called: not bound |
| `toupper` | `CrtToupper` | nothing | bound: 2,081 | not called: not bound | not called: not bound |
| `strncpy` | `CrtStrncpy` | ranges of `count` bytes that are not plain memory | bound: 4,436 | bound: 31,436 | bound: 85 |
| `strncat` | `CrtStrncat` | strings that reach the register window, a written range that overlaps the source or is not plain memory | bound: 517 | bound: 14 | bound: 36 |
| `wcscpy` | `CrtWcscpy` | a source that reaches the register window, a destination that overlaps it, ranges that are not plain memory | bound: 1 | bound: 2,171 | not called: not bound |
| `wcsncpy` | `CrtWcsncpy` | ranges of `count` elements that are not plain memory | not called: not bound | bound: 1,154 | not called: not bound |

The figures are the calls compared with the function's own code in one run of each title with
`--any360_xdk_verify=true` (Test Drive Unlimited and Crackdown: 3.0 M calls of all the bound functions, to their
title screens and first menu; The Simpsons Game: 184 M, its menus and a load), 0 different in the three. A function
no run of a title has called is not bound there, whatever its tests say: `wcscpy` in Test Drive Unlimited and
`strrchr` in The Simpsons Game rest on 1 and 5 calls, and on the unit tests.

Three of them write a range whose size the registers do not give (`strncat`, `wcscpy`, `wcsncpy`): each has a plan
(`CrtStrncatPlan` ...) that says, before anything is written, whether the function takes the call and which bytes
it will write, so that the check in game can keep those bytes and the ones next to them for both sides.

These functions are called little (thousands of calls where `memcpy` has millions) and the hand-written ones are
no faster than their models for `strchr` and `strncmp` (each element costs a test of the register window): they
are here to take the library off the recompiled code, not for speed, and are not tuned.

To find which bound function a title's trouble comes from: `--any360_xdk_skip="strncpy,wcscpy"` leaves the named
ones to their own code and the others native, to be halved until one is left. `tools/callee_registers.py
<generated dir> <address>...` lists the volatile registers a title's code reads after calling a function before
writing them itself (a replacement gives r3 and the memory, not what the function's code happened to leave in
r8 or cr0); for the functions bound here it finds only instructions that do nothing (`mr r8,r8`).

`memmove` and the forward copy are the same instructions in the three titles; the vector copy is three builds of
the same steps (other registers, the same tests), read in each. Its name is not in the titles: it is the function
next to XAPI that copies 16-byte vectors, asks for the source's cache lines ahead and zeroes the destination's
128-byte cache lines ahead of writing them, which is what the XDK's `XMemCpy` does. Its model
(`xapi_reference.h`) follows those steps, zeroing included: the unit tests show that for two ranges apart it
writes the destination's bytes and nothing else, and that for ranges that overlap it leaves what neither `memmove`
nor a forward copy leaves (the zeroed line held source bytes not read yet), which is why those calls stay its own
code's.

Crackdown and The Simpsons Game had their own copies of these functions; Crackdown's are gone, and The Simpsons
Game keeps its own only for a build without Any360. Two of those copies were wrong where the shared function is
right: The Simpsons Game's `memcpy` saved the destination's address and not its whole register, and its vector
copy took ranges that overlap from below as a plain copy. The Simpsons Game's copy of whole 64-byte blocks is in
no library and in neither of the other titles: the title's own code, it stays there. Not bound: the forward copy
and the vector copy in Crackdown's Title Update 6 guest (its code is not on this machine).

### The check in game

The code as it is, every call done both ways and compared (`--any360_xdk_verify=true`), 2026-11-10:

| Title | Calls compared | `memcpy` | forward copy | `memset` | `memmove` | vector copy | Different | Reached |
|---|---|---|---|---|---|---|---|---|
| Test Drive Unlimited | 3.0 M | 1.86 M | 2 | 1.14 M | 7 | 24 | 0 | its title screen |
| Crackdown | 2.0 M | 1.16 M | 2 | 778 k | 2.5 k | 61 k | 0 | its title screen |
| The Simpsons Game | 83 M | 49.1 M | 211 k | 11.3 M | 1.3 k | 22.4 M | 0 | a level |

Start was not pressed in the first two on that last run. The three functions bound first were checked earlier the
same day with those titles played: Test Drive Unlimited past its title screen (14 M calls), Crackdown in the city
(21 M), 0 different; their code has not changed since.

`memmove` and the vector copy are called little outside The Simpsons Game: their check in the two other titles
rests on few calls, and on the unit tests.

What the check found that the unit tests had not: `memcpy` saves the 64 bits of its destination register below the
stack pointer, and Test Drive Unlimited calls it with a register whose high half is not zero (13,856 calls in
13.7 M). The model had the high half as zero, so the tests passed on a wrong model. The model, the function and a
unit test (`SavedRegister`) now have it.

The bytes next to a destination changed during a few calls (Test Drive Unlimited: 254 in 3 M while `memcpy`'s own
code ran and 139 while the native one ran, all to one 256-byte buffer; The Simpsons Game: 80 and 56 in 83 M):
another thread writing its own data there. A function that wrote outside its destination would show at
every such call, on its own side only.

### The measure in game

The function's own recompiled code against the native one, on every call (`--any360_xdk_bench=true`):

| Title | `memcpy` | forward copy | `memset` | `memmove` | vector copy |
|---|---|---|---|---|---|
| Test Drive Unlimited | 70.0 -> 13.1 ns a call (5.3x), 13.2 M calls | 686 -> 32.7 ns (21x), 73 k calls | 59.2 -> 21.3 ns (2.8x), 713 k calls | 7 calls | 24 calls |
| Crackdown | 41.5 -> 14.7 ns (2.8x), 29.7 M calls | | 30.4 -> 13.9 ns (2.2x), 38.3 M calls | 123 -> 43 ns (2.8x), 2.5 k calls (title screen) | 171 -> 40 ns (4.3x), 61 k calls (title screen) |
| The Simpsons Game | 22.7 -> 9.8 ns (2.3x), 43.2 M calls | 79.3 -> 14.7 ns (5.4x), 2.05 M calls | 55.9 -> 11.3 ns (4.9x), 8.7 M calls | 74 -> 10 ns (7.1x), 1.3 k calls | 88.2 -> 15.2 ns (5.8x), 22.1 M calls |

What these figures say and do not say:

- They carry the cost of reading the clock and of memory that is not in the cache (a call of 0 bytes reads 9 to
  22 ns): they compare the function's own code with the native one, and cannot tell two native versions apart. The
  benchmark does that.
- Over 100 to 150 seconds of play, the natives save about 0.8 second of processor time in Test Drive Unlimited,
  1.4 in Crackdown and 2.7 in The Simpsons Game (1.6 of it the vector copy, which that title calls 170,000 times
  a second for 1 to 2 KB): 1 to 2% of one thread. No gain in frames was measured.
- For Crackdown and The Simpsons Game the comparison is with the recompiled code, not with what they ran before:
  their own copies already used the host's `memmove` and `memset`. What they gain is the tested, shared function
  and the paths for small sizes.

### The titles' sizes and what optimization kept

| Title | Copies (`memcpy`) | Fills (`memset`) | Vector copies |
|---|---|---|---|
| Test Drive Unlimited | 16 bytes: 50%, 64: 32%, 48: 9% | 120 bytes: 65%, 64: 12%, 128: 5% | too few |
| Crackdown | 104 bytes: 28%, 12: 14%, 64: 10%, 40: 7%; a third between 65 and 128 | 16 bytes: 43%, 48: 35% | a third each of 16 bytes, 64 bytes and 4 to 8 KB (title screen) |
| The Simpsons Game | 64 bytes: 50%, 12: 39%, 192: 3% | 64 bytes: 59%, 1 to 2 KB: 18%, 48: 8% | 1 to 2 KB: 97% |

`xdk_crt_bench`, each title's mix with its sizes in random order, the version first written (the host's `memmove`
and `memset` behind the first tests of plain memory) against the code as it is:

| | Test Drive Unlimited | Crackdown | The Simpsons Game | The three |
|---|---|---|---|---|
| `memcpy` | 1.12x | 1.33x | 1.15x | 1.2x |
| `memcpy` with the host's `memmove` alone behind the current test | 1.14x | 1.10x | 1.15x | 1.13x |
| `memset` | 1.34x | 1.56x | 1.18x | 1.3x |

- Kept: the one test of plain memory for both ranges of a copy (most of `memcpy`'s gain); for `memcpy`, sizes up
  to 128 bytes as overlapping loads and stores (they add Crackdown's gain and nothing elsewhere); for `memset`,
  sizes up to 128 bytes as overlapping stores. No title is slower than with the first version.
- Taken out: one path with no branch on the size for 16 to 64 bytes (0.99x for `memcpy` on Test Drive Unlimited's
  mix, no better than the branches for `memset`); and, for the vector copy, the hand-written short copies
  (`xdk_xapi_bench`: 0.97x on the titles' mixes, whose copies are long): it is the host's `memmove` alone.
- What one title would have got wrong: on Test Drive Unlimited's mix alone, the part of the `memcpy` path from 65
  to 128 bytes gained nothing and was taken out. Crackdown, a third of whose copies are that size, gains from it
  and neither of the others loses: it is back. An optimization is settled only when every title at hand has been
  measured.
- `memmove`'s own side (the backward copy) is called a few thousand times in two minutes at most: measured by size
  in `xdk_crt_bench` (the same copy as `memcpy`'s), not tuned.

### Found on the way

- Crackdown crashed after 40 to 55 seconds with a gamepad connected and the input group on `any360::sys`: its
  host code (the button-glyph overlay) and Any360's input sampling thread both called the runtime's input system,
  which takes one caller at a time. `any360::host::rexglue::LockHostInput` is the lock a title's host code takes
  around such a call; the overlay takes it.
- Crackdown's title screen, left without a frame limit (more than 1,500 frames a second), does not take the Start
  button; at 60 frames a second it does. Not looked into: it is the title's port, not a native function.
- Crackdown's start took 32 seconds with the files on `any360::sys` (a file read one byte at a time, the host file
  opened for each byte): docs/SYS_BINDING.md, step 2.
- The cure for that start (a handle keeps its host file open) broke The Simpsons Game for a night: with it, the
  host's kernel reads straight into guest memory, and it does not write to pages the renderer has protected to see
  the title's writes; a level's files read as 0 bytes. The native functions were suspected first (they were bound
  the same evening) and cleared by `--any360_xdk_natives=false`, then by `--any360_xdk_skip`. The cause, the fix and
  the tools that found it: docs/SYS_BINDING.md, step 2.
- The Simpsons Game called `XamContentCreate` with an empty file name and "create always": the content path was
  then its type's folder, emptied with every save in it. A package with an empty name is refused now
  (`Xam::ContentPath`).
- The Simpsons Game's menus run at 22 frames a second instead of 30 on some launches, the window shown (30 every
  time minimized): the renderer's thread waits in the presentation of each frame. Not looked into here: it is
  presentation, not a native function.
