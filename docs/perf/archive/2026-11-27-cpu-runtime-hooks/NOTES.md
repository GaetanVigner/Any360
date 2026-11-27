# 2026-11-27: Any360's PowerPC translator, the instructions that need the runtime

The batch "runtime hooks" of the translator's parallel work (docs/CPU.md): what a translated function cannot do on its
own and asks the runtime for, written as macros the including code defines, the way slice 2 made `bctrl`
`ANY360_CPU_CALL_INDIRECT(ctx, base, target)`:

- calls to imports (kernel and XAM functions): `ANY360_CPU_CALL_IMPORT(ctx, base, <library>, <ordinal>)`;
- `mfmsr` / `mtmsrd`, the global lock: `ANY360_CPU_CHECK_GLOBAL_LOCK()`, `ANY360_CPU_ENTER_GLOBAL_LOCK()`,
  `ANY360_CPU_LEAVE_GLOBAL_LOCK()`;
- `mftb` / `mftbu`, the time base: `ANY360_CPU_QUERY_TIMEBASE()`;
- traps (`tw`, `twi`, `td`, `tdi` and the simplified forms the titles use: `twllei`, `twlgei`, `tdllei`, `tdlgei`):
  `ANY360_CPU_TRAP(ctx, base, <type>)`.

Nothing is bound in a title yet; no instruction id is marked native.

Machine: cores 0-1 (`start /affinity 3`, ninja -j 2), clang 22.1 -O2 (RelWithDebInfo), Windows 11; other sessions
built and benchmarked on the other cores at the same time.

## What is translated

A title runs whole functions, so the share that counts is theirs (a function translated with all its callees). Each
line adds or removes one part of the batch (`share_parts.txt`):

| | Mirror's Edge functions | its instructions | Crackdown functions | its instructions |
|---|---:|---:|---:|---:|
| slice 2 (before) | 15,644 of 53,832 (29.1%) | 13.9% | 11,567 of 41,219 (28.1%) | 10.6% |
| the global lock, time base and traps only | 15,652 (29.1%) | 13.9% | 11,578 (28.1%) | 10.6% |
| calls to imports only | 16,256 (30.2%) | 14.7% | 12,133 (29.4%) | 11.4% |
| this batch (both) | **16,272 (30.2%)** | **14.7%** | **12,156 (29.5%)** | **11.4%** |
| this batch, with `lwarx` / `stwcx.` assumed translated | 16,330 (30.3%) | 14.7% | 12,184 (29.6%) | 11.4% |

Mnemonics covered: 131 listed, 125 of them used by the titles (of 352), 97.0% of their static occurrences (96.9%
before).

The gain is the imports': every call to an import's thunk now translates (1,135 calls in Mirror's Edge were blocked by
them, 0 now). `mfmsr` blocked 1,630 calls, but every `mfmsr` / `mtmsrd` section of the titles brackets a `lwarx` /
`stwcx.` pair (the kernel's interrupt-free atomic update), so those functions now stop at `lwarx` (1,596 blocked
calls), an atomic of the scalar-rest batch; even with the atomics assumed, only 58 more Mirror's Edge functions (28
Crackdown) translate whole, their callers having other blockers (`bctr`, `lbzu`, `divw` ...). Traps sit mostly in
front of `divw` / `divwu` (the compiler's divide-by-zero check), also scalar-rest's. Blockers now, Mirror's Edge, by
the calls they block: `lwarx` 1,596, `bctr` 1,565, `lbzu` 1,388, `fcfid` 790, functions that run into the next one 534,
VMX128 472, `divw` 449, `subfe` 433, `divwu` 396, `fsqrts` 320.

## Tests

- `cpu_isa_tests.txt`: 69 cases (7 new), 282,624 states, all match. The new cases, against expectations written by
  hand from the ISA and the hooks' definitions, the recorded handlers' calls compared in order:
  - `msr`: `mfmsr r3; mtmsrd r13,1; mfmsr r5; mtmsrd r3,1; mfmsr r6` (lock free, taken, released); `mtmsrd_mask`
    (`mtmsrd r4,1` alone: a release without a take);
  - `timebase`: `mftb`, `mftbu` (the handler's time base has a high word);
  - `trap_immediate`, `trap_other`, `trap_register`: the four opcodes, the titles' forms, TO 0, TO 31,
    `twi 31,r0,<type>`, and a value with a zero low word and a non-zero high word (1 << 32), which tells the word
    forms from the doubleword ones;
  - `imports`: `bl` to an import's thunk, a conditional and an unconditional branch to others (tail calls).
  Each new mnemonic is in `COVERED_MNEMONICS` (the build checks it has a case). A known difference, checked by name:
  `mtmsrd r4,1` of 0xFFFFFFFF gives msr 0x8020 (EE and IR) and releases the global lock; the ISA's mask is 0x8002
  (EE and RI).
- `cpu_e2e_tests.txt`: Mirror's Edge, 319 functions (10,661 instructions; 458 with their callees) of the 12,908 both
  versions take (12,386 before), 20,416 runs, 1,223 stopped at their bound: all match the reference, registers, msr,
  memory and the hooks' calls in order. 300 are every k-th as before; `cpu_e2e.py` now adds, of any size, up to 16
  functions whose own code calls each hook, which every k-th rarely picks (the first set had 42 import calls and no
  other hook): 16 for imports, 3 for traps, 1 for the time base, 0 for the global lock (no function with `mfmsr` /
  `mtmsrd` translates whole yet). In the runs: 1,659,968 import calls, 256 traps, 128 time base reads (both versions
  counted). The global lock is tested by the ISA cases only until the atomics land; the e2e test then picks those
  functions by itself.
- The two sides name an import from independent sources: the translator reads library and ordinal from the thunk's
  words and the XEX's import libraries header; `tools/ppc_model.py` maps the recompiled `__imp__<name>(ctx, base)`
  through `db/system_calls.json` (every import name of both titles is there). So the e2e test checks that the right
  import is called.
- Mutation campaigns (`mutants.py`, `mutate_lines.py`, `reach.py`):
  - `mutants_hooks.txt` (`--hooks`: 20 of the 85 constants on the hooks' lines, all import ordinals as drawn): 13
    caught; the 7 missed are on 6 lines no state reaches (`reach.txt`).
  - `mutants_targeted.txt`, the trap and time base lines by hand: 3 of 5 caught (a trap's type 20 -> 21, 22 -> 0, the
    time base + 1); the 2 missed are on branches the states do not take (a trap after a call, and the second `mftb`
    of a function that reads it again only when the low word read is 0).
  - `mutants.txt` (20 constants anywhere, seed 2): 11 caught; of the 9 missed, 7 on lines no state reaches, 2 reached
    (a compare with 106, an `addi` of 4) where the made-up data does not tell the mutant apart.

## Benchmark (`cpu_e2e_bench_run1-3.txt`, `cpu_e2e_bench 5`)

The 319 functions (316 timed: for 3, no state ends within the bounds), up to 16 states each, 64 rounds, fastest of
5; the versions without steps. Both versions call the same recorded handlers; each timed run starts them from the same
state.

| run | Any360 ns (sum of a call of each) | ReXGlue ns | speed | geometric mean of the 316 speeds | B / allocations a call |
|---|---:|---:|---:|---:|---|
| baseline: empty function, same context copy and call | 7.2 - 12.7 | | | | 0 / 0 |
| 1 | 31,478.2 | 28,169.6 | 0.89x | 1.006x | 0 / 0 |
| 2 | 38,122.7 | 31,843.6 | 0.84x | 0.993x | 0 / 0 |
| 3 | 34,299.4 | 36,302.9 | 1.06x | 0.995x | 0 / 0 |
| best of 3 per function | 23,283.6 | 22,934.3 | 0.985x | 0.998x | 0 / 0 |

Parity. The sums move with a few heavy functions that run up to 2.3x slower in one run than in another, on both
sides (sub_82C796B8: 4,860 to 10,993 ns); 44 functions are below 0.95x and 54 above 1.05x in the best of 3 (5 and 1 in
slice 2). This bench ran on 2 cores while other sessions built and benchmarked on the rest of the machine (shared
cache and memory bandwidth), where slice 2's ran on a quiet 8 cores. The 20 functions picked for the hooks: 0.93x to
1.14x, 1.00x median. The hooks add no work of their own beside the recompilation's: the same statements around the
same macro, whose cost is the runtime's on both sides.

## Choices

- Each hook is a macro the including code defines, as `ANY360_CPU_CALL_INDIRECT`: the translated code stays free of
  any runtime; a title binds it to `any360::sys` and its runtime, the tests to recorded handlers (`cpu_hooks.h`)
  that fold every call and its arguments into one trace compared between the two versions (a hook called with
  another ordinal, type, order or count fails the test), keep their state per run (reset before each version's run,
  runs stopped inside a locked section included), never block (a release without a take is allowed: made-up data does
  that), and leave a mark in r3 and in memory for an import call, like an indirect call's.
- Imports are named by library and ordinal, not by name: the thunk holds only those, and `any360::sys` binds by them
  (`db/system_calls.json` has every export's ordinal). The library is a bare token (`xboxkrnl`, `xam`), so a title can
  paste it into a name or dispatch on it at compile time. Thunks are found by their words in the image (the XEX
  loader's records `0x01LLOOOO 0x02LLOOOO`, then `mtctr r11; bctr`), like the save and restore helpers. `--xex`
  (`translate_tree`'s new argument `libraries`, none by default) gives the library names, so the other batches' calls
  and the share scripts keep working as before.
- The statements follow ReXGlue's builders: the full fence around `mfmsr` / `mtmsrd`, msr's mask 0x8020, the lock
  taken on `mtmsrd r13,1` and released on any other register, `mfmsr` returning 0x8000 when the lock is free, the
  trap conditions in their order and literal forms, `twi 31,r0,<type>` passing its type, a call to an import resetting
  the flush mode like any call. The lock by register (not by the EE bit written) matches the kernel's idiom (save with
  `mfmsr rX`, disable with `mtmsrd r13,1`, restore with `mtmsrd rX,1`) and keeps nested sections balanced on a
  recursive lock, which a rule on the EE bit would not (a nested restore writes EE = 0 and would take it again). msr
  is now in ppc_model.h's `Ctx` (0x200A000 at start, as in the reference runtime), drawn at random in the tests and
  compared.
- `mtmsrd` with L = 0 and `mftb` of other time base registers are left out (no title uses them).

## Next

The global lock sections pay off with the atomics (`lwarx`, `stwcx.`), the traps with the divides (`divw`, `divwu`),
both in the scalar-rest batch; the e2e test will pick their functions by itself. Binding a title needs its definitions
of the hooks (`any360::sys` by library and ordinal, the runtime's lock, time base and trap handler).
