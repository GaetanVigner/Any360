# CPU: Any360's PowerPC translator

The titles' code runs today as ReXGlue recompiled it: every CPU instruction id is `reference` on the progress page,
which counts as not migrated (CLAUDE.md). Any360's own translator reads a title's functions from the PowerPC words of
its XEX image and writes them as C++ on `any360/xdk/ppc_model.h`'s context (`Ctx`: the registers; `Memory`: the guest
memory), with ReXGlue's recompilation kept only as the oracle its output is compared with.

- `tools/ppc_translate.py`: the decoder and emitter. `--image` / `--functions` translate functions of a title (with
  their callees; `--xex` names the libraries of its calls to imports); `--cases` translates the instruction test cases.
- `tools/cpu_e2e.py`: the end-to-end set: a title's functions both versions take, translated by Any360 and recompiled
  by ReXGlue (`tools/ppc_model.py`), written into the build folder (nothing of the title is kept in the repository).
- `layer/cpu`: the tests and the benchmark, built with `layer/sys` and run by `layer\sys\test.ps1`.

## What is translated

Whole functions are what a title can run translated, so the share that counts is theirs: a function is translated
only when all its instructions are covered and every function it calls is translated too.

| | slice 1 (2026-11-26) | slice 2 (2026-11-27) | runtime hooks (2026-11-27) |
|---|---:|---:|---:|
| Mirror's Edge functions | 6,514 of 53,832 (12.1%) | 15,644 (29.1%) | 16,272 (30.2%) |
| Mirror's Edge instructions in them | 2.4% | 13.9% | 14.7% |
| Crackdown functions | 4,631 of 41,219 (11.2%) | 11,567 (28.1%) | 12,156 (29.5%) |
| Crackdown instructions in them | 1.3% | 10.6% | 11.4% |
| mnemonics covered (of the 352 the titles use) | 52 | 120 | 125 |
| their static occurrences in the titles | 86.7% | 96.9% | 97.0% |

What keeps the other functions out, by the calls from otherwise translated functions it blocks (Mirror's Edge, after
the runtime hooks): `lwarx` (1,596; with `stwcx.`, the atomics inside the `mfmsr` / `mtmsrd` sections), `bctr` (1,565;
switch tables, and tail calls through a pointer), `lbzu` (1,388), `fcfid` (790), functions that run into the next one
(534), the vector unit (472; VMX128, opcode 4: `lvx128`, `stvx128` ...), `divw` (449), `subfe` (433), `divwu` (396),
`fsqrts` (320). Every call to an import is now translated.

## Instructions covered

| Instructions | Encoding | Translation |
|---|---|---|
| `addi`, `li`, `addis`, `lis` | D, opcodes 14 / 15 | `rD = rA + SIMM` (64-bit, SIMM << 16 for `addis`), `rD = SIMM` when rA is 0 |
| `addic`, `addic.`, `subfic`, `mulli` | D, opcodes 12 / 13, 8, 7 | the 64-bit sum, difference or product; CA the carry out of the low word |
| `ori`, `oris`, `xori`, `andi.` (`nop`) | D, opcodes 24-26, 28 | 64-bit; `andi.` sets cr0; `ori 0,0,0` is nothing |
| `lbz`, `lhz`, `lwz`, `stb`, `sth`, `stw`, `stwu` | D, opcodes 34, 40, 32, 38, 44, 36, 37 | big-endian loads (zero-extended) and stores; `stwu` writes the address back into rA's low word |
| `ld`, `std` | DS, opcodes 58 / 62 | big-endian 64-bit load / store |
| `lfs`, `lfd`, `stfs`, `stfd` | D, opcodes 48, 50, 52, 54 | single to double on load, double to single on store (rounded), doubles as their bits |
| `lwzx`, `lbzx`, `lhzx`, `stwx`, `stbx`, `sthx`, `lfsx`, `stfsx` | X, opcode 31 | as the D forms, the address rA + rB (rB when rA is 0) |
| `cmpwi`, `cmplwi`, `cmpdi`, `cmpldi`, `cmpw`, `cmplw`, `cmpd`, `cmpld` | D opcodes 11 / 10, X opcode 31 xo 0 / 32 | the field's lt / gt / eq, so from XER |
| `rlwinm` (`clrlwi`, `rotlwi` ...), `rlwimi` | M, opcodes 21 / 20 | the low word rotated in a doubled 64-bit value, masked by MASK(mb + 32, me + 32); `rlwimi` keeps rA outside the mask |
| `rldicl` (`clrldi`, `rotldi`), `rldicr`, `rldimi` | MD, opcode 30 | the 64-bit rotation masked by MASK(mb, 63), MASK(0, me), MASK(mb, 63 - sh) |
| `or`, `mr`, `and`, `andc`, `xor`, `nor`, `not` | X, opcode 31 | 64-bit |
| `add`, `subf`, `neg`, `mullw`, `addze` | XO, opcode 31 | 64-bit; `mullw` the product of the signed low words; `addze` adds CA, CA the carry out of the low word |
| `extsb`, `extsh`, `extsw`, `cntlzw` | X, opcode 31 | sign extension; the leading zeros of the low word |
| `slw`, `srw`, `srawi` | X, opcode 31 | the low word shifted, 0 for amounts 32-63; `srawi` sets CA when a negative value loses one bits |
| record forms (`.`) of the above | Rc = 1 | cr0 from the low word as a signed compare with 0, so from XER |
| `dcbt`, `dcbf`, `sync`, `lwsync`, `eieio` | X, opcode 31 | nothing (cache hints and barriers of the guest's memory model) |
| `fmr`, `fneg`, `fabs`, `frsp` | X, opcode 63 | the bits, the sign flipped or cleared; rounded to single |
| `fcmpu` | X, opcode 63 xo 0 | lt / gt / eq, so set when unordered (a NaN) |
| `fadds`, `fsubs`, `fmuls`, `fdivs`, `fadd`, `fsub`, `fmul`, `fdiv` | A, opcodes 59 / 63 | the double result, rounded to single for the `s` forms |
| `fmadds`, `fmsubs`, `fnmsubs`, `fmadd`, `fmsub` | A, opcodes 59 / 63 | `std::fma` (one rounding to double), then rounded to single for the `s` forms |
| `mflr`, `mtlr`, `mfctr`, `mtctr` | XFX, opcode 31 xo 339 / 467 | lr / ctr |
| `b`, `bl` | I, opcode 18 | a jump inside the function; `bl` sets lr and calls the callee's translation |
| `bc` forms (`beq`, `bne`, `blt` ..., `bdnz`, `bdz`, `bdnzf`, `bdnzt`, `bdzf` ...) | B, opcode 16 | the condition of BO / BI; when BO asks, ctr (64-bit) decremented first and its low word tested |
| `bclr` forms (`blr`, `beqlr` ..., `bdzlr`) | XL, opcode 19 xo 16 | a return, conditional as above |
| `bctrl` | XL, opcode 19 xo 528 | lr set, then `ANY360_CPU_CALL_INDIRECT(ctx, base, ctr)`: the title's function table in a title, a recorded handler in the tests |
| `mfmsr` | X, opcode 31 xo 83 | a full fence, then rD = `ANY360_CPU_CHECK_GLOBAL_LOCK()` (0x8000 when the runtime's global lock is free, else 0) |
| `mtmsrd rS,1` | X, opcode 31 xo 178, L = 1 | a full fence, msr's bits 0x8020 from rS, then `ANY360_CPU_ENTER_GLOBAL_LOCK()` when rS is r13, else `ANY360_CPU_LEAVE_GLOBAL_LOCK()` |
| `mftb`, `mftbu` | XFX, opcode 31 xo 371, TBR 268 / 269 | rD = `ANY360_CPU_QUERY_TIMEBASE()` (the guest's 50 MHz time base), its high word for `mftbu` |
| `tw`, `twi`, `td`, `tdi` (`twllei`, `twlgei`, `tdllei`, `tdlgei` ...) | X opcode 31 xo 4 / 68, D opcodes 3 / 2 | `ANY360_CPU_TRAP(ctx, base, 0)` when one of the TO field's conditions holds (signed <, >, ==, unsigned <, >; the low words for `tw` / `twi`), nothing for TO 0, always for TO 31; `twi 31,r0,imm` passes imm as the trap's type |

Beside the instructions:

- A branch to another function's start (a tail call, `b` or a conditional branch) becomes that function's translation
  and a return. A branch to the function's own start is a loop.
- A call or branch to the ABI's register save and restore helpers (`__savegprlr_n`, `__restgprlr_n`, `__savefpr_n`,
  `__restfpr_n`), which no function table lists, is recognized by their code and becomes `SaveGprLr` / `RestGprLr` /
  `SaveFpr` / `RestFpr` of ppc_model.h.
- A call or branch to an import's thunk (the XEX loader's words `0x01LLOOOO 0x02LLOOOO`, then `mtctr r11; bctr`:
  library LL of the XEX's import libraries header, ordinal OOOO) becomes `ANY360_CPU_CALL_IMPORT(ctx, base, <library>,
  <ordinal>)`, for example `ANY360_CPU_CALL_IMPORT(ctx, base, xboxkrnl, 304)` (`RtlLeaveCriticalSection`): a title binds
  it to `any360::sys` by library and ordinal. `--xex` gives the translator the library names; without it, calls to
  imports are left out as before.
- What needs the runtime is a macro the including code defines, like `ANY360_CPU_CALL_INDIRECT`:
  `ANY360_CPU_CALL_IMPORT`, `ANY360_CPU_CHECK_GLOBAL_LOCK`, `ANY360_CPU_ENTER_GLOBAL_LOCK`,
  `ANY360_CPU_LEAVE_GLOBAL_LOCK`, `ANY360_CPU_QUERY_TIMEBASE`, `ANY360_CPU_TRAP`. The tests define them as recorded
  handlers (`layer/cpu/tests/cpu_hooks.h`) whose every call and argument, in order, both versions must match.
  `tools/ppc_model.py` turns the recompiled code's `__imp__<name>(ctx, base)` (library and ordinal from
  `db/system_calls.json`), `REX_*_GLOBAL_LOCK()`, `REX_QUERY_TIMEBASE()` and `ppc_trap(ctx, base, n)` into the same
  macros, so both versions call the same handlers.
- The floating-point flush mode follows the titles' recompilation: before the first floating-point instruction
  (loads and stores included) after a label, a call or an indirect call, the translation sets the host's MXCSR to keep
  denormals (`fpscr.disableFlushMode()`).

`tools/ppc_translate.py --cases` stops the build when a mnemonic of `COVERED_MNEMONICS` (the list the roadmap marks
`translated`) has no case in `isa_cases.txt`; `tools/cpu_e2e.py` stops it when a function the translator takes names an
instruction that is not in that list, or when the two versions of a function differ in their steps or their
flush-mode changes.

### Where the translation differs from the ISA

It follows the titles' recompilation, which the tests compare it with; `cpu_isa_tests` checks each of these by name:

- `fmadds`, `fmsubs`, `fnmsubs`: rounded twice (to double by `std::fma`, then to single), where the ISA rounds once.
  641 x 6700417 x 2^-56 + 1 gives 0x3F800000; the ISA gives 0x3F800001.
- A NaN made by an operation (inf - inf, 0 x inf ...) is the host's default NaN, 0xFFF8000000000000; the PowerPC's is
  0x7FF8000000000000.
- `stfs` of a value that is not a single rounds it; the ISA truncates it (1/3 as a double: 0x3EAAAAAB, the ISA
  0x3EAAAAAA).
- `mtmsrd rS,1` copies the bits 0x8020 of rS into msr (EE and IR), where the ISA copies EE and RI (0x8002); the
  interrupts it enables or disables are the runtime's global lock instead, taken when rS is r13 (the kernel's
  `mtmsrd r13,1` that disables interrupts, r13 holding the processor block's address) and released for any other
  register (the `mtmsrd rX,1` that puts back what `mfmsr rX` read); `mfmsr` reads the lock, not msr.

Changing these makes the translation differ from what the titles run today: it is a decision for the in-game binding,
not for the translator alone.

## Tests

- `cpu_isa_tests`: each case of `layer/cpu/tests/isa_cases.txt` (69 cases, encoded words ending in `blr`; `import`
  lines make addresses import thunks for the cases) translated at build time, against the ISA's definition of the
  same instructions written by hand in the test (Power ISA 2.06 Book I; the runtime hooks' calls, in order, as the
  recorded handlers see them), on 4,096 register states each (282,624 in all), integer edge values included; floating-point cases draw
  their operands from edge values (zeros, infinities, a NaN, denormals, values that round or overflow). Each
  floating-point instruction is the first one of a case, so that its flush-mode change is checked. Every register and
  every byte of guest memory compared.
- `cpu_e2e_tests`: the end-to-end set (300 of the 12,908 Mirror's Edge functions both versions take, spread over the
  image, and up to 16 more for each runtime hook, of the functions whose own code calls it: 319 functions, 10,661
  instructions, 458 with their callees), each run from 64 register states on two guest spaces that start with the
  same memory; every register and every byte either version touched, and every runtime hook's call in order, must
  match. Runs on made-up
  data are bounded: both versions pass `ANY360_CPU_STEP()` at each function entry and label and `ANY360_CPU_ENTER()` at
  each entry, and a run that passes 16,384 steps or nests 256 calls stops there, in both at the same point. Built only
  where the title's files are (`titles/<title>/pipeline.json`: `image`, `generated`; `docs/data/functions-<title>.json`);
  `ANY360_CPU_E2E_TITLE` picks the title.
- `cpu_e2e_bench`: each function of the set, Any360's translation against ReXGlue's recompilation, without the steps:
  64 rounds over the states on one memory, every timed run starting from the same memory, on the states whose every
  call of the rounds ends within the bounds; ns a call and host allocations a call.

## Done

An instruction id turns `native` only when Any360's translation of it runs in a title: the title's functions bound to
their translations, checked in game (0 different) and measured there. Until then the roadmap shows it as `translated
(not bound)`. Binding also needs what the recompilation does at run time and the translator does not yet: memory-mapped
I/O addresses (`REX_MM_` loads and stores), the function table behind `ANY360_CPU_CALL_INDIRECT`, and the title's
definitions of the runtime hooks: `ANY360_CPU_CALL_IMPORT` on `any360::sys` by library and ordinal, the global lock and
the time base on the runtime's, traps on its trap handler.

## Measures

The results are in [perf/archive/2026-11-26-cpu-translator-slice1](perf/archive/2026-11-26-cpu-translator-slice1/),
[perf/archive/2026-11-27-cpu-translator-slice2](perf/archive/2026-11-27-cpu-translator-slice2/) and
[perf/archive/2026-11-27-cpu-runtime-hooks](perf/archive/2026-11-27-cpu-runtime-hooks/) (NOTES.md), and
indexed in [perf/FUNCTIONS.md](perf/FUNCTIONS.md).
