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

1. Find the function in the titles (`tools/shared_functions.py tdu crackdown simpsons --leaf` for the ones that
   are the same instructions in all, `tools/library_scan.py`, the call counts of a trace) and look its address up
   in each title's `titles/<title>/modules.json`: it must sit in a range of kind `library`. The same instructions
   in every title is not enough: Havok is in all three, and its own `strlen` and `strcmp` were bound for a few
   hours as if they were a library's (State, below). Then read its generated
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

Five functions that copy and fill, the same code in the three titles at hand, then twenty-four more of the C
runtime and XAPI (below). Unit tests: `xdk_crt_tests` (2,486,548 checks) and `xdk_xapi_tests` (1,780,296 checks).

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

### The comparisons, mbtowc and the last error (2026-11-11)

Six more, from the functions that call nothing among those that are the same instructions in the three titles
(172 of the 602), most call sites first. Models and unit tests as the others; `xdk_crt_bench` has a
second table for every string function that reads or writes a length.

| Function | Library | Handler | Declines | Test Drive Unlimited | Crackdown | The Simpsons Game |
|---|---|---|---|---|---|---|
| `wcscmp` | CRT | `CrtWcscmp` | a string that reaches the register window | not called: not bound | bound: 9,442,359 calls | bound: 2 |
| `strlen`, a loop of bytes | Havok (middleware) | `CrtStrlen` | a string that does not end in plain memory | not called; not bound | checked: 15; not bound | not called; not bound |
| `strcmp`, a loop of bytes | Havok (middleware) | `CrtStrcmp` | the same | checked: 9,387; not bound | not called; not bound | not called; not bound |
| `mbtowc` (the "C" locale) | CRT | `CrtMbtowc` | a source in the register window, a destination that is not plain memory | not called: not bound | bound: 110,633 | not called: not bound |
| `GetLastError` | XAPI | `XapiGetLastError` | a control region or a thread that is not plain memory | checked: 171,899; not bound | checked: 73,195; not bound | checked: 4,819; not bound |
| `SetLastError` | XAPI | `XapiSetLastError` | the same | checked: 171,972; not bound | checked: 70,297; not bound | checked: 50,734; not bound |

The check in game (every call both ways, 0 different): Test Drive Unlimited 5.0 M calls, Crackdown 46 M (in the
city), The Simpsons Game 151 M (two minutes of a level). `strchr`, `strncmp` and `strrchr`, changed that day (the
span, below), were in those runs again.

The two loops of bytes were a mistake of mine, put right the same day. They were picked for being the same
instructions in the three titles and bound (Crackdown's `strlen`, Test Drive Unlimited's `strcmp`) before anyone
looked at where they sit: inside Havok's code (`titles/tdu/modules.json`; at the head of Havok's range in
Crackdown). Havok is middleware, kept as it is and never replaced function by function, so they are bound in no
title. They stay in `crt.h` with their models and tests for the value they give: the C runtime's own `strlen` and
`strcmp` are not the same instructions in the three titles, and each will need its build read. A function is
looked up in the title's module map before it is picked: `kind` must be `library`.

`GetLastError` and `SetLastError` are five instructions each. Checked, they are right; measured, they gain nothing
as host routines (below): the call through the binding costs what the loads cost. They are in `xapi.h` with their
models and tests and bound in no title. `mbtowc`, `tolower` and `toupper` gain nothing either (1.0x) and lose
nothing: they stay bound where they were checked.

### `_stricmp` and `strstr` (2026-11-11)

The two string functions of the C runtime with the most call sites that were left (Crackdown: 308 and 63; The
Simpsons Game: 171 and 52; Test Drive Unlimited: 68 and 31). They are the same instructions in the three titles;
the comparison of whole functions had missed them because the generated code of some builds carries padding after
a function's last return (`tools/shared_functions.py`, the comparison kept from then on, stops there and
says which module each function sits in). All four addresses that have a module are in the C
runtime; Test Drive Unlimited's `_stricmp` sits in the same unnamed tail as its `wcscmp`.

| Function | Handler | Declines | Test Drive Unlimited | Crackdown | The Simpsons Game |
|---|---|---|---|---|---|
| `_stricmp` | `CrtStricmp` | a string that does not end in plain memory | bound: 9,943 calls | bound: 651,270 | bound: 103,203 |
| `strstr` | `CrtStrstr` | the same | bound: 2 | bound: 152,901 | bound: 557 |

The figures are the calls compared in game, 0 different (Test Drive Unlimited 3.0 M calls of all the bound
functions, Crackdown 50 M in its city, The Simpsons Game 168 M in its menus and loads). `_stricmp` leaves at the
second string's end before it lowers anything, so "B" against "" is 66 and not 98: the model and the function
have it, a unit test checks it. `strstr` is the host's search once both strings are known to end in plain memory.

`xdk_crt_bench`, against their models: `_stricmp` 1.4 to 2.3 times faster, `strstr` 1.5 times (4 bytes) to 17
times (2,048 bytes, the needle absent). In game (`--any360_xdk_bench=true`, title screens and menus): `_stricmp`
38.4 -> 23.9 ns (1.6x, 217 calls) in Test Drive Unlimited, 10.8 -> 8.2 ns (1.3x, 477 k) in Crackdown, 26.5 ->
19.7 ns (1.3x, 56 k) in The Simpsons Game; `strstr` 100 -> 22 ns (4.6x, 41 k) in Crackdown, 466 -> 320 ns (1.5x,
8 calls) in The Simpsons Game, and 2 calls in Test Drive Unlimited, where the first one pays the host routine's
first use (360 -> 1,510 ns): too few to say anything.

### The helpers on a double's bits (2026-11-11)

Six functions of the C runtime that take a double apart by its bits, with no arithmetic: `_finite`, `_isnan`,
`_sptype` (which infinity or NaN), `_get_exp`, `_set_exp` and `_add_exp` (the names of the C runtime's own
sources; the math functions call them). The same instructions in the three titles, all in their C runtime ranges.
They take their argument in f1, which the host's part did not pass before: a call now carries the bits of f1,
and the two that return a double leave it in f1.

Each stores the double in the caller's home area at r1 + 16 first (`stfd f1,16(r1)`), which is what the check in
game compares as the bytes written; `_set_exp` and `_add_exp` build their result in the 16 bytes below the stack
pointer, and `_add_exp` leaves the double at r1 - 8, the slot the check compares for every leaf function. The
functions do the same stores. Their first instruction being a store of a double, the function's own code turns the
host's flush-to-zero mode off there: the handlers do too.

| Function | Handler | Test Drive Unlimited | Crackdown | The Simpsons Game |
|---|---|---|---|---|
| `_finite` | `CrtFinite` | not called: not bound | not called: not bound | bound: 704 calls |
| `_isnan` | `CrtIsnan` | not called: not bound | bound: 92 | not called: not bound |
| `_sptype` | `CrtSptype` | not called: not bound | not called: not bound | not called: not bound |
| `_get_exp` | `CrtGetExp` | bound: 18,103 | bound: 1,134,237 | bound: 16,462 |
| `_set_exp` | `CrtSetExp` | bound: 18,103 | bound: 1,134,237 | bound: 16,462 |
| `_add_exp` | `CrtAddExp` | not called: not bound | not called: not bound | not called: not bound |

The figures are the calls compared in game, 0 different (Test Drive Unlimited 6.0 M calls of all the bound
functions, Crackdown 51 M in its city, The Simpsons Game 89 M with a level). Unit tests: 60,000 doubles (the
infinities, both kinds of NaN, zeros, denormals, the largest and smallest normal ones, then any 64 bits) against
the models and against what the host's own functions say of the same double (`isfinite`, `isnan`, `ilogb`,
`frexp`, `ldexp`). Crackdown had `_add_exp` written by hand among its own functions, giving the same bits: it
keeps it, the shared one not being bound in a title that has not called it.

They gain nothing as host routines and lose nothing: 6 to 17 instructions each. `xdk_crt_bench`: 1.1 to 1.7 ns
a call. In game (`--any360_xdk_bench=true`): `_get_exp` 8.8 -> 9.0 ns (1.0x), 16.8 -> 15.3 (1.1x), 8.0 -> 7.8
(1.0x) in the three titles in that order, `_set_exp` 10.8 -> 9.7 (1.1x), 15.6 -> 13.2 (1.2x), 8.0 -> 8.2 (1.0x),
`_finite` 13.8 -> 9.9 (1.4x, 296 calls) in The Simpsons Game: about what a call costs to time. Like `tolower`,
they are bound where they were checked, to take the library off the recompiled code.

Left of the C runtime's leaf functions that are the same in the three titles: the ones that read and write the
floating-point status register (`mffs` / `mtfsf`: the control word's functions), which need the runtime's model of
that register and are not written.

### The measure in game, the string functions and these (2026-11-11)

`--any360_xdk_bench=true`, the function's own code against the host routine, nanoseconds a call (calls):

| Function | Test Drive Unlimited | Crackdown | The Simpsons Game |
|---|---|---|---|
| `strchr` | 21.8 -> 8.7 (2.5x), 911 k | 22.6 -> 14.5 (1.6x), 46.5 k | 14.0 -> 11.1 (1.3x), 1.7 k |
| `strrchr` | 152 -> 53 (2.9x), 80 | 55.9 -> 29.8 (1.9x), 1.3 k | 88 -> 43 (2.0x), 36 |
| `strncmp` | 13.8 -> 9.9 (1.4x), 33.7 k | 16.0 -> 14.0 (1.1x), 15.3 k | 49 -> 35 (1.4x), 52 |
| `strncpy` | 95 -> 52 (1.8x), 8.0 k | 99.8 -> 21.9 (4.6x), 55.3 k | 198 -> 49 (4.1x), 253 |
| `strncat` | 34.8 -> 19.2 (1.8x), 538 | 121 -> 83 (1.4x), 104 | 48.7 -> 35.6 (1.4x), 36 |
| `wcslen` | 171 -> 104 (1.6x), 253 | 32.4 -> 28.9 (1.1x), 306 k | |
| `wcscpy` | 2 calls | 42.6 -> 31.2 (1.4x), 11.0 k | |
| `wcsncpy` | | 79.4 -> 24.6 (3.2x), 5.1 k | |
| `wcscmp` | | 12.8 -> 9.5 (1.3x), 9.54 M | 2 calls |
| `strlen` (bytes, Havok's: no longer bound) | | 108 -> 68 (1.6x), 15 | |
| `strcmp` (bytes, Havok's: no longer bound) | 39.0 -> 10.2 (3.8x), 8.7 k | | |
| `tolower` | 8.3 -> 7.8 (1.1x), 4.7 k | 8.0 -> 7.7 (1.0x), 581 k | |
| `toupper` | 7.9 -> 7.7 (1.0x), 2.1 k | | |
| `mbtowc` | | 11.2 -> 11.2 (1.0x), 135 k | |
| `GetLastError` | 7.7 -> 7.8 (1.0x), 172 k | 15.3 -> 14.4 (1.1x), 80 k | 48.9 -> 91.8 (0.5x), 2.0 k |
| `SetLastError` | 7.9 -> 8.5 (0.9x), 172 k | 13.5 -> 13.9 (1.0x), 77 k | 49.9 -> 72.5 (0.7x), 36 k |

About 8 ns is what a call costs to time at all. The one function here that a title calls much is `wcscmp` in
Crackdown (53,000 calls a second in the city): 1.3x of 12.8 ns, about 0.2% of one thread. The Simpsons Game's
figures for the last error were taken with one count for all functions deciding which side runs first; two
functions always called one after the other may each have kept the same place. The order is by function now
(not measured again).

### What the benchmark changed: one test of the memory a string, not one a byte

The functions that read a string a byte at a time tested every byte for the GPU's register window. `xdk_crt_bench`
(the table of the string functions by length) had them 1.2 to 1.3 times slower than their own models: `strchr`
51 ns against 47 for 64 bytes, `strlen` 67 against 55, `strcmp` 35 against 29. `PlainSpan(address)` says once how
many bytes from an address are plain memory under one host pointer (to the register window, to the physical view,
to the end of the address space); the function reads that far with no other test, and a string that has not
ended there is left to the function's own code. After: `strchr` 31 ns, `strcmp` 19, `strncmp` 23 (was 32),
`strrchr` 52 (was 71), and `strlen`, which is then the host's search for a zero byte, 5.8 ns (131 ns for 2,048
bytes against 1,685 for its model). Unit tests: the spans at each boundary, and strings that have not ended at
the window's edge, declined.

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

Two things the check itself got wrong, found on 2026-11-11 with Crackdown in its city and The Simpsons Game in a
level, and how it runs since:

- It ran the function's own code, put the destination back as it was, then ran the replacement. For an instant
  the destination held its old bytes again. A title fills a structure on one thread while another draws it: The
  Simpsons Game crashed 37 seconds into a level, every call verified and none different, its main thread reading a
  pointer that was gone again. Most calls read nothing of what they write (fills, copies whose source is apart);
  for those the replacement now runs first on the memory as it is and the function's own code over it, and what
  they write never goes back. The few others (a copy onto its own source, `strncat`'s terminator) still have the
  destination put back between the sides. The same level, 151 M calls: no crash. That the bytes put back were the
  cause is what the evidence points to, not proven.
- Two sides that differ are run again on the same input, three times at most. A function that is wrong for an
  input is wrong each time; when the sides then agree, another thread wrote the source or the destination between
  them. Crackdown's vector copy differed 3 times in 2.0 M calls (16 and 12 bytes copied from globals of the
  image): run again, all three agreed, in two runs. The log counts those calls apart.

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

### Where the processor time goes (2026-11-11)

Measured after 29 functions, to know what is worth porting next: `tools/cpu_profile.ps1` samples the threads of a
running title about a thousand times a second each and puts on every sample the processor cycles the thread used
since its last one. Titles in play, everything on Any360, 45 seconds each:

| | Test Drive Unlimited (driving, 30 FPS) | Crackdown (city, 60 FPS) |
|---|---|---|
| Processors used | 2.23 | 2.12 |
| Waits and sleeps (the system calls, with what ran just before them) | 27.9% | 44.6% |
| The title's recompiled code | 46.8% | 28.6% |
| of which the C runtime | 0.3% | under 0.5% |
| of which the D3D runtime | 34.4% | 3.2% |
| of which XAudio | | 5.9% |
| of which game code | 9.0% | 15.8% |
| Any360 (graphics almost all of it) | 8.8% | 11.6% |
| The port's host code | 7.7% | 6.3% |
| GPU driver | 2.4% | 2.2% |

The Simpsons Game was measured in its menus and on a loading screen only (55 to 77% in waits and sleeps, the C
runtime 0.4%); in a level it has not been.

What it says:

- The C runtime is 0.3 to 0.5% of the cycles. Nothing ported in this file can show in a frame rate, and nothing
  left in the C runtime can either. The functions are here to take the library off the recompiled code.
- A third to a half of the cycles go to threads that wake and sleep again: a wait with a short time-out in a loop.
  By the function that asked: any360::sys's waits (`ObjectManager::WaitForObject` sleeps 1 ms at a time and looks
  again: 5 to 18% of the cycles), the renderer's worker loops (`VulkanRenderer::RecordLoop` 5 to 8%,
  `AsyncRenderer::Run`, `SubmitLoop`), Crackdown's two XAudio threads (a quarter of its cycles between them,
  mostly waking and sleeping), the runtime's timer thread (2 to 9%).
- Test Drive Unlimited's main thread is one processor, and 83% of it is the D3D runtime waiting for the GPU by
  spinning (`sub_826800D8` calling `sub_826737B8`: eight `db16cyc`, four times, then a look at the ring's read
  pointer), at an even 30 frames a second: it waits for its swap, not for work.

The first of those was taken on the same day: any360::sys's waits no longer look every millisecond
(docs/SYS_BINDING.md, step 3). Wake-ups fell by four to seven times; the processor saved is small (0.07 of one in
Crackdown's menus), most of what the profile put on those waits being the signals themselves and the code that ran
before them.

So the processor time is in how threads wait, in Any360's own layers and in the D3D runtime, not in library leaf
functions. The cycles a thread carries when found at a wait include what it ran just before: the shares above
are upper bounds for the waits themselves.

The D3D runtime's spin was taken next: "The D3D runtime's GPU wait", below.

### The D3D runtime's GPU wait (2026-11-11)

Every wait of the D3D runtime for the GPU (room in the ring, the end of a frame, a fence, a query) is a loop in
the caller around one function, the wait's step: eight `db16cyc` pauses, then the device's stop flag, the
ring's read pointer and the time, and a hang check after about five seconds without progress. It returns nonzero
while the wait goes on; the caller looks at its word between two steps. On the console the pauses give the
hardware thread's time to the other one; recompiled they are nothing, and the loop takes a processor for as long
as the GPU needs. Each of the four titles at hand has exactly one such function, the same steps at device
offsets of its runtime's version: Test Drive Unlimited `sub_826737B8` (eight callers), The Simpsons Game
`sub_82452018`, Crackdown `sub_826BBEE0`, Call of Duty: Black Ops `sub_824B3F38`, each in its `XDK D3D runtime`
range (library) of `titles/<title>/modules.json` (Black Ops has no module map yet).

`any360::xdk::D3dGpuWait` (`layer/xdk/include/any360/xdk/d3d.h`) and its binding
`any360::host::rexglue::xdk::D3dGpuWaitStep` keep the step's own code. When it says the wait goes on, the
thread sleeps until the GPU's progress count moves, at most `any360_d3d_wait_longest_us` (2 ms), so the step's
own checks still run. The count (`CommandProcessor::Progress`) moves after every write the guest can see of the
GPU: the read pointer written back, fences, memory writes and query counts, in stream order or after the draws,
and interrupt callbacks; two of those (writes in stream order, the query counts) did not move it before and do
now. The count a thread sleeps on is the one it read after its previous step, before the caller looked at its
word again: no write is slept through. `any360_d3d_wait_sleep=false` gives the spin back. While the graphics are
not bound to Any360 the step is its own code alone.

It is not a replacement that computes the same thing faster, so the rule's check of both sides does not apply.
What stands for it: `xdk_d3d_tests` (every order of the look, the write and the step on one thread; 20,000 waits
raced against a writer, with and without other progress; two waiters; a lost wake would show as a wait of five
seconds), `gfx_cp_tests` (3,000 waits on a memory write and the read pointer, against the command processor
itself, with and without a renderer: none reaches its time-out), and the titles in play.

Test Drive Unlimited driving, the same build, 30 seconds of `tools/cpu_profile.ps1` each (the two runs on
different stretches of road):

| | the step spins (`any360_d3d_wait_sleep=false`) | the step sleeps |
|---|---|---|
| Frames a second | 30 | 30 |
| Processors used by the process | 1.86 | 1.31 |
| The thread that waits | 0.98 of a processor | 0.21 |
| Steps a second | 23 million | 800 to 1,000, a third of them to the 2 ms time-out |
| The D3D runtime's share of the cycles | 18.6% | 1.3% |

The spinning run counted its steps with a look at the clock each (44% of the thread, which spins either way);
the count now looks at the clock once every 4,096 steps. The profile of the morning (45 s, before the waits of
any360::sys changed) had 2.23 processors with the step spinning.

The Simpsons Game's port already slept in this step on Any360's progress (its `ring_wait.cpp`, the same steps
with 1 ms); it now calls the shared binding (menus at 30 frames a second, 60 to 100 steps a second, all slept).
Crackdown's port has its own hook on the step (`gpu_fence_wait.cpp`): it blocks on the very word the GPU writes
for the three waits that wait for one, and keeps the others spinning; left as it is. Black Ops does not bind it
yet.

### Past the C runtime: XAudio's mix and D3D's constants (2026-11-11)

The C runtime is under half a percent of the titles' cycles; the profiles' library functions above it are in the
audio library and the D3D runtime. The two taken here:

- **XAudio's mix with a volume ramp** (`xdk::XAudioMixRamp`, `layer/xdk/include/any360/xdk/xaudio.h`): Crackdown
  `sub_82A4B6A0` (2.4% of its cycles in the city profile, the audio threads' hottest function), the same
  instructions in Test Drive Unlimited (`sub_826B50B8`). destination += source x gain, four vectors of four samples
  at a time, the gain advancing by a step a vector in four running sums. A vector kernel: the replacement takes
  and gives back the registers the function writes (`VmxRegisters`: r3-r11, v0-v13, v29-v31, r31's save at r1 - 8),
  and its model (`xaudio_reference.h`) does the 64 instructions one element at a time. vmaddfp is fused, one
  rounding, as the console and Crackdown's recompiled code do it (the host's FMA3, the same instruction and operand
  order, under the vector unit's flush mode); a host without FMA3 declines every call. Test Drive Unlimited's
  build of the SDK compiles vmaddfp as a multiply then an add (two roundings): the replacement would differ from
  its recompiled code in the last bit, and the game barely calls it: not bound there.
- **D3D's upload of the dirty shader constants** (`xdk::D3dDirtyConstants`, `d3d.h`): Crackdown `sub_826C5EA0`,
  Test Drive Unlimited `sub_8268DEF0`, the same steps in another order with the command buffer's pointer at
  device + 48 and + 40 (the binding's parameter). For each run of set bits in a 64-bit mask: type-2 pads to 16
  bytes, a header ((words - 1) << 16 | index), the run's 64-byte constants. A run without room goes to the
  runtime's function that makes room: the replacement plans the whole call first and declines it then. The Simpsons
  Game's runtime has a different function for this.

Checks: `xdk_xaudio_tests` (12,952: every count of samples to 200 and long ones, unaligned addresses, 64-bit
registers, denormals, infinities, NaNs, overlapping buffers, declines) and `xdk_d3d_tests` (39,605 with the GPU
wait: 412 masks, every alignment of the buffer's pointer, sources unaligned, both offsets, a run without room,
overlaps). In game with `--any360_xdk_verify`: Crackdown 237,124 mixes and 207,223 uploads, Test Drive Unlimited
156,818 uploads, none different (registers included for the mix).

In game with `--any360_xdk_bench`, the calls' own mix of sizes (the mix in Crackdown's menus; the uploads in play,
after the benchmark's order was fixed, below):

| | own code | replacement | |
|---|---|---|---|
| Crackdown, the mix (256 samples, 1.9 million calls) | 144-183 ns | 81-91 ns | 1.8-2.0x |
| Crackdown, the upload (3.0 million calls, in the city) | 60 ns | 32 ns | 1.9x |
| of which one constant (66%) | 40 ns | 24 ns | 1.7x |
| of which 64 constants | 488 ns | 214 ns | 2.3x |
| Test Drive Unlimited, the upload (1.5 million calls, driving) | 129 ns | 53 ns | 2.5x |
| of which one constant (15%) | 40 ns | 23 ns | 1.7x |
| of which 64 constants | 387 ns | 102 ns | 3.8x |

What they are worth: Crackdown's menus mix about 4,700 times a second, 0.3 ms a second saved; its city about
455,000 times a second (measured 2026-11-12, 14.9 million calls: 161 -> 83 ns a call, 1.9x), 73 -> 38 ms of a
processor a second, about 3.5% of one saved. The uploads are once
or twice a draw: about 0.1 ms a frame in Test Drive Unlimited's driving, a little less in Crackdown's city.

Found on the way:

- **The benchmark's order was biased.** Which side runs first alternated call by call of a function. Crackdown
  uploads constants twice a frame (the vertex shader's, then the pixel shader's), so the second call was always on
  the same side: the replacement always ran first, and it alone paid the page fault below. It measured 0.1x (7.6
  microseconds against 0.5) where the two sides are equal. The order is now the count times the golden ratio's
  fraction: half and half with no period. A call ten times slower than the function's own code is logged with its
  arguments and run again at once (the second run shows whether the time was the replacement's or the memory's).
- **Faults on Crackdown's command buffer, the first seconds only.** A draw reads data the title put in the command
  buffer itself, so the renderer copies that page to video memory and watches it; the CPU then goes on writing
  commands into the same page, and that write takes a fault (40,000-80,000 ticks, 10-20 microseconds, whoever
  writes it). Logged (each watch the renderer arms over the buffer, 100 s in the menus): 20 to 24 a second during
  the first five seconds, one more at 37 s, then none: the renderer stops watching a page the CPU wrote three times
  (`kHotPageWrites`), and the buffer's pages are each written once a lap. Nothing to change. The benchmark's report
  and the log of slow calls had both looked at those first seconds only (the report comes at the 200,000th timed
  call). Copying and watching only the pages a batch reads (not the gaps under 64 KB copied along) was tried and
  left out: these pages are read, and it gained nothing measured.

### D3D's dirty words, and both uploads planned at once (2026-11-12)

**D3D's upload of 4-byte constants** (`xdk::D3dDirtyWords`, `d3d.h`): the dirty-constants upload's sibling for the
constants of one word a bit (Crackdown `sub_826C5B00`, Test Drive Unlimited `sub_8268DB50`, Call of Duty: Black Ops
`sub_824B20F0`, the same steps; The Simpsons Game `sub_8245E8C8`). Per run of set bits a header and the run's words,
no pads; its room test leaves the header out. The Simpsons Game's port patched its copy of the function by hand (it
makes each page writable before writing to it: its write pointer can land in read-only memory): a replacement would
bypass the patch, and it is not bound there.

**Both uploads planned in constant time.** A replacement plans a call before writing, so that it can decline it
whole. The first version walked the runs twice (plan, then write) with a test of plain memory a run: on the host
it was slower than the step-by-step model for small masks (one word: 8.3 ns against 2.7). The write pointer only
grows, so each run's room test is stricter than the one before and the last run's decides them all; where the last
run starts follows from the mask alone (its set bits, its runs: set bits whose next higher bit is clear, the length
of the lowest run), and after the first run of 64-byte constants every header takes 3 pads. The plan is now a few
bit counts, then one pass writes. `xdk_d3d_bench`: one word 8.3 -> 4.7 ns, 32 runs of words 139 -> 71 ns, 32 runs
of constants 232 -> 161 ns. Checks: `xdk_d3d_tests` 79,184 (the words' masks and alignments as the constants', the
room test's edge, a second run without room).

In game, verified (none different): Crackdown 804,915 uploads of words and 381,458 of constants, Test Drive
Unlimited 817,387 and 508,704. Bench (menus, the fixed order):

| | own code | replacement | |
|---|---|---|---|
| Crackdown, the words (620,000 calls) | 51 ns | 33 ns | 1.6x |
| Crackdown, the constants (287,000 calls) | 70 ns | 38 ns | 1.8x |
| Test Drive Unlimited, the words (1.4 million calls) | 38 ns | 25 ns | 1.5x |
| Test Drive Unlimited, the constants (1.6 million calls) | 140 ns | 52 ns | 2.7x |

### D3D's copy of the shader constants, and where Crackdown's city stands (2026-11-12)

**The copy of a title's shader constants into the device** (`xdk::D3dSetShaderConstants`, `d3d.h`; Crackdown
`sub_826B2F68`, Test Drive Unlimited `sub_8266B808`, the same steps in another order): `count` vectors of 16 bytes
from any alignment (lvlx and lvrx) to the device's copy of the constant registers, four then one at a time, then
the dirty bits or-ed into the device's first doubleword; it saves r7 in its caller's parameter area. A copy that
reads over what it writes is declined. `xdk_d3d_tests`: 86,021 checks with it (every count to 70 and long ones,
every alignment of the source, the device on and off 16 bytes, 64-bit registers, the declines). In game, verified:
Crackdown 329,139 calls, Test Drive Unlimited 1,687,731, none different. Bench: Crackdown 44 -> 27 ns (1.7x),
Test Drive Unlimited 18.0 -> 14.3 ns (1.3x, 8.4 million calls; its most frequent copy, one vector, gains only
0.7 ns: the checks of plain memory cost about what the copy saves, about 0.1 ms a second in all, left as it is).

**Crackdown's city with today's functions** (`tools/cpu_profile.ps1`, 45 s, 2026-11-12): the process used 1.33
processors (2.12 in the profile of the day before, another stretch of the city, with the waits and every
function of this file since then). XAudio fell from 5.9% of the cycles to 0.85%, all the libraries together to
about 4%: the biggest library function left is XAudio's effects chain (`sub_82A4DCF0`, 0.40%), then D3D's
`sub_826C3750` (0.37%). The rest: waits and sleeps 49%, the game's own code 13%, Any360's graphics 15% (the
command processor's thread, the renderer's draws and write-backs). The library functions have given most of what
they had to give; what costs now is Any360's graphics and the game's own code.

### Shared library functions not ported yet, and XAudio's fills (2026-11-12)

Past the profiles' hot spots, the library functions the three titles share instruction for instruction
(`tools/shared_functions.py tdu crackdown simpsons --leaf`) and Any360 has not ported, most called first: an
Xbox Live helper (1,054 call sites: a single player game hardly runs it), the C runtime's `_controlfp`-like FPSCR
helper (77: it would need the guest's floating-point state, left), XAPI and D3D wrappers that tail-call another
function, five XAudio fills, small D3D functions of a format or a fetch constant (no loop: a host routine gains
nothing on them, as GetLastError showed).

**XAudio's fills** (`xdk::XAudioFill`, `xaudio.h`): a block's buffer of 256, 512, 1024, 2048 or 4096 floats set to
the block's float, then the block's first word to 0; five builds of the same steps, the same in the three titles.
lfs and stfs move a single's bits as they are on the console: the replacement copies the bits (the recompiled
code converts to double and back, the same bits for every float but a signalling NaN). `xdk_xaudio_tests`: 13,555
with them (the five sizes, any alignment, signalling NaNs and denormals, declines). In game: Crackdown calls them 64
times (at start), verified none different, 1,327 -> 86 ns a call (15.4x); Test Drive Unlimited and The Simpsons
Game did not call them in their menus: not bound there.

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
- The check in game crashed a title by itself (the bytes it put back between its two sides) and called another
  thread's write a difference: "The check in game", above.
- The Simpsons Game's menus run at 22 frames a second instead of 30 on some launches, the window shown (30 every
  time minimized): the renderer's thread waits in the presentation of each frame. Not looked into here: it is
  presentation, not a native function.
