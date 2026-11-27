# 2026-11-27: Any360's PowerPC translator, second slice

The next units of the roadmap's CPU area, most used first: the scalar instructions after slice 1 (`lbz`, `stb`, `lwzx`,
`subf`, `ori`, `fmr`, `lfd`, `fsubs`, `fmadds`, `fcmpu`, `fadds`, `lhz`, `sth` and the forms beside them), `bctrl`, and
what blocked the most whole functions once those were in: tail calls, the ABI's register save and restore helpers, the
64-bit rotates and `addze`. Nothing is bound in a title yet; no instruction id is marked native (docs/CPU.md, "Done").

Machine: cores 0-7 (`start /affinity FF`), clang 22.1 -O2 (RelWithDebInfo), Windows 11.

## What is translated

A title runs whole functions, so the share that counts is theirs (a function translated with all its callees):

| | slice 1 | slice 2 |
|---|---:|---:|
| Mirror's Edge functions | 6,514 of 53,832 (12.1%) | 15,644 (29.1%) |
| Mirror's Edge instructions in them | 2.4% | 13.9% |
| Crackdown functions | 4,631 of 41,219 (11.2%) | 11,567 (28.1%) |
| Crackdown instructions in them | 1.3% | 10.6% |
| mnemonics (of the 352 the titles use) | 52 | 120 |
| their static occurrences | 86.7% | 96.9% |

How the share grew, Mirror's Edge (in the order of the work): the scalar instructions 17.7%; with `bctrl` 20.6%; with
tail calls 22.4%; with the save and restore helpers 28.4%; with the 64-bit rotates, `addze` and the barriers 29.1%.
`bctrl` alone was the only instruction left in 12,390 functions; the helpers were most of the 16,129 functions that
branch outside themselves (`b __restgprlr_n` at their end).

## Tests

- `cpu_isa_tests.txt`: 62 cases, 253,952 states, all match the ISA. Each floating-point mnemonic is the first
  floating-point instruction of its own case, so its flush-mode change is compared. Floating-point operands come from
  edge values (zeros, infinities, a NaN, float denormals, values that round or overflow); the arithmetic cases keep
  frB finite and not zero, so that no operation makes a new NaN (the sign of a made NaN is one of the known
  differences below).
- The known differences from the ISA, each checked by name in `cpu_isa_tests` (the translation follows the titles'
  recompilation, which the e2e test compares it with):
  - `fmadds` 641 x 6700417 x 2^-56 + 1: 0x3F800000 (rounded to double by `std::fma`, then to single); the ISA rounds
    once: 0x3F800001.
  - `fadd` inf + -inf: 0xFFF8000000000000, the host's default NaN; the PowerPC's is 0x7FF8000000000000. The check passes
    its operands through a `volatile`: with constants, clang folds the addition at build time and gives the positive
    NaN.
  - `stfs` of the double 1/3: 0x3EAAAAAB (rounded); the ISA truncates: 0x3EAAAAAA.
- Mutation check of the ISA test: without the `stfs` flush flag (the slice 1 defect below), the `stfs` case fails on
  the flush mode and on the stored value.
- `cpu_e2e_tests.txt`: Mirror's Edge, 300 functions (10,898 instructions; 425 with their callees) of the 12,386 both
  versions take, 19,200 runs, 1,197 stopped at their bound (16,384 steps or 256 calls deep), 1,239,044 indirect calls:
  all match the reference, registers and memory. The set has 405 `bctrl`, 7 tail calls, 87 / 119 GPR save / restore
  helper calls, 5 / 7 FPR ones, 213 fused multiply-adds.
- `tools/cpu_e2e.py` stops the build when a function the translator takes names an instruction outside
  `COVERED_MNEMONICS` (it found `fdiv`, decoded but not listed: added with its case), and when the two versions of a
  function differ in their steps or their flush-mode changes.
- `mutants.txt` (`mutants.py 20 2`): 20 mutants of the translated header, one integer constant + 1 each, rebuilt and
  run: 12 caught. Of the 8 missed (`reach.py`: an `abort()` inserted before the line), 5 are on lines no state
  reaches; 1 is equivalent (`<< 32` to `<< 33` under a mask of 0x3); 2 are reached but the made-up data does not tell
  them apart: a byte reached through two pointers compared with 1 (the data rarely puts 1 or 2 there), and a vtable
  load whose word and the next lead to the same call target. The share caught is lower than slice 1's (14 of 20):
  the set is twice the instructions, with more code behind branches that random states do not take.

## Benchmark (`cpu_e2e_bench_run1-3.txt`, `cpu_e2e_bench 5`)

The 300 functions (297 timed: for 3, no state ends within the bounds), up to 16 states each, 64 rounds, fastest of
5; the versions without steps. Each timed run starts from the same memory.

| run | Any360 ns (sum of a call of each) | ReXGlue ns | speed | geometric mean of the 297 speeds | B / allocations a call |
|---|---:|---:|---:|---:|---|
| baseline: empty function, same context copy and call | 7.3 - 9.4 | | | | 0 / 0 |
| 1 | 17,550.2 | 17,658.7 | 1.01x | 0.991x | 0 / 0 |
| 2 | 18,710.1 | 18,556.8 | 0.99x | 0.999x | 0 / 0 |
| 3 | 17,712.2 | 17,570.8 | 0.99x | 0.999x | 0 / 0 |
| best of 3 per function | 17,288.7 | 17,139.2 | 0.99x | 0.997x | 0 / 0 |

Parity. 5 functions below 0.95x, 1 above 1.05x. The four slowest (sub_8264ED78 0.87x, sub_82D06D00 0.92x,
sub_8234A190 0.94x, sub_82CF9790 0.94x) differ from the recompilation in the same 5 statements each (a byte load,
two byte stores, two carry tests), all only in a literal's suffix (`+ 0u` against `+ 0`, `> 0u` against `> 0`): the
same unsigned operation. The gap is put down to code placement, not to what the translation computes; the assembly was not
compared. A run of the bench before these outputs (same binary but for the instruction count in the header line):
1.010x, 1.001x, 1.012x.

The bench totals are not comparable with slice 1's: the 300 functions are another set (10,898 instructions against
5,122).

## Defect fixed from slice 1

`stfs` did not set the flush mode: ReXGlue's recompiler sets it before every floating-point instruction, loads and
stores included (`emit_set_flush_mode(false)` in its memory and floating-point builders), after a label or a call. A
block whose first floating-point instruction was an `stfs`, entered with flush on, kept flush on in the translation.
Slice 1's e2e runs did not meet it; its notes and CPU.md state the rule right ("before the first floating-point
instruction"), the code left `stfs` out of it.

## Choices

- The recompilation's statements where it departs from the ISA (the three above), as in slice 1 (cr0 from the low
  word, `stwu`): the e2e test, the parity bench and the in-game check (0 different) are all defined against it, and
  rounding fused singles once costs a branch and an error term a call. Console-exact against recompilation-exact is
  decided at the in-game binding (docs/CPU.md).
- `bctrl` becomes `ANY360_CPU_CALL_INDIRECT(ctx, base, target)`, defined by the code that includes the header: the
  title's function table at binding, a recorded handler in the tests (`cpu_indirect.h`: r3 from the target and lr, a
  word stored, a count). `tools/ppc_model.py` turns ReXGlue's `REX_CALL_INDIRECT_FUNC` into the same macro, so both
  versions call the same handler.
- Tail calls and helpers follow ReXGlue's classification: a branch to another function's start is a call and a
  return; the helpers are found by their code (the words of `std rN ... stw r12,-8(r1); blr` and the like), as the
  function tables do not list them.
- Runs on made-up data are now bounded in call depth too: tail calls between functions recurse in the host's stack
  (the first run overflowed it, 0xC00000FD, before reaching the step bound).
- `bctr`, the vector unit (`lvx128` ...), `mfmsr` / `mtmsrd`, traps, `lbzu` and `fcfid` are left for the next slices
  (docs/CPU.md lists what blocks the most calls).

## Next

By the calls they block: `mfmsr` / `mtmsrd` (with the runtime's global lock), `bctr` (switch tables), `lbzu` and the
other update forms, imports (the title calling `any360::sys`), `fcfid` / `fctiwz`, the vector unit. Then a title binds
translated functions and checks them in game.
