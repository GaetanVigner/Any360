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

| | slice 1 (2026-11-26) | slice 2 (2026-11-27) | runtime hooks (2026-11-27) | bctr (2026-11-27) | scalar rest (2026-11-28) |
|---|---:|---:|---:|---:|---:|
| Mirror's Edge functions | 6,514 of 53,832 (12.1%) | 15,644 (29.1%) | 16,272 (30.2%) | 17,321 (32.2%) | 30,907 (57.4%) |
| Mirror's Edge instructions in them | 2.4% | 13.9% | 14.7% | 15.3% | 32.6% |
| Crackdown functions | 4,631 of 41,219 (11.2%) | 11,567 (28.1%) | 12,156 (29.5%) | 13,138 (31.9%) | 14,832 (36.0%) |
| Crackdown instructions in them | 1.3% | 10.6% | 11.4% | 12.3% | 17.4% |
| mnemonics covered (of the 352 the titles use) | 52 | 120 | 125 | 128 | 243 |
| their static occurrences in the titles | 86.7% | 96.9% | 97.0% | 97.0% | 97.6% |

What keeps the other functions out, by the calls from otherwise translated functions it blocks (Mirror's Edge, after
the scalar rest): functions that run into the next one (2,998), the vector unit (1,115; VMX128, opcode 4: `lvx128`,
`stvx128` ...), branches out of the function to anywhere but a function start (648; into the middle of another
function), jump tables whose targets are not labels of the function (219), words that are not instructions (154;
data in the code), `mffs` (27). The scalar rest took in the atomics of the global-lock sections (`lwarx` / `stwcx.`):
together with the runtime hooks they let most of Mirror's Edge's lock-holding code through.

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
| `lbzu`, `lhzu`, `lhau`, `lwzu`, `ldu`, `stbu`, `sthu`, `stdu`, `lfsu`, `lfdu`, `stfsu`, `stfdu` | D / DS, opcodes 35, 41, 43, 33, 58, 39, 45, 62, 49, 51, 53, 55 | the access at rA + d, then that address into rA's low word (rA 0, and rA = rD for loads, left out as invalid forms) |
| `lbzux`, `lhzux`, `lhaux`, `lwzux`, `lwaux`, `ldux`, `stbux`, `sthux`, `stwux`, `stdux`, `lfsux`, `lfdux`, `stfsux`, `stfdux` | X, opcode 31 | as the D forms, the address rA + rB |
| `lha`, `lhax`, `lwa`, `lwax`, `ldx`, `stdx`, `lfdx`, `stfdx`, `stfiwx` | D / DS / X | the algebraic loads sign-extended; `stfiwx` stores frS's low word |
| `lwbrx`, `lhbrx`, `stwbrx`, `sthbrx` | X, opcode 31 | little-endian access (the loads zero-extended) |
| `lwarx`, `ldarx`, `stwcx.`, `stdcx.` | X, opcode 31 | the reserving load keeps the word, as it lies in memory, in `ctx.reserved`; the conditional store is a compare-and-swap of that value with rS, cr0 = 0, 0, stored, so |
| `dcbz`, `dcbzl`, `dcbtst`, `dcbst`; `or rN,rN,rN` (`db16cyc`, `cctpl`, `cctpm`) | X, opcode 31 | zero the 32-byte block, the 128-byte line; the others nothing |
| `andis.`, `xoris`, `orc`, `eqv`, `nand` | D / X | 64-bit; `andis.` sets cr0 |
| `sld`, `srd`, `sraw`, `srad`, `sradi`, `cntlzd` | X / XS, opcode 31 | 0 for amounts 64-127; `sraw` / `srad` take amounts over 31 / 63 as 31 / 63, CA set when a negative value loses one bits |
| `rlwnm` (`rotlw`), `rldic` | M opcode 23, MD opcode 30 | the low word rotated by rB's low 5 bits, masked; the 64-bit rotation masked by MASK(mb, 63 - sh) |
| `addc`, `adde`, `addme`, `subfc`, `subfe`, `subfme`, `subfze` | XO, opcode 31 | the 64-bit sum; CA the carry out of the low word, carry in included |
| `mulhw`, `mulhwu`, `mulhd`, `mulhdu`, `mulld` | XO, opcode 31 | the high word sign- / zero-extended; the high doubleword of the 128-bit product |
| `divw`, `divwu`, `divd`, `divdu` | XO, opcode 31 | the quotient (the word ones zero-extended); 0 for a zero divisor and for the most negative value by -1 |
| `mcrf`, `crand`, `crandc`, `creqv`, `crnand`, `crnor`, `cror`, `crorc`, `crxor`, `mfcr`, `mfocrf`, `mtcrf` (`mtcr`), `mtxer` | XL opcode 19, XFX opcode 31 | the fields and bits of the condition register; `mfocrf` the first field FXM names, the other bits 0 |
| `fcfid`, `fctiw`, `fctiwz`, `fctid`, `fctidz` | X, opcode 63 | the conversions; a NaN gives the most negative integer (as a 32-bit value zero-extended for the word ones), values over the largest integer it (over or equal for the word ones) |
| `fsqrt`, `fsqrts`, `fres`, `frsqrte`, `fsel`, `fnabs`, `fnmadd`, `fnmadds`, `fnmsub` | A / X, opcodes 59 / 63 | `std::sqrt`; the reciprocal and reciprocal square root exact and rounded to single; frA >= 0 ? frC : frB; the sign set; `-std::fma` |
| `bctrl` | XL, opcode 19 xo 528 | lr set, then `ANY360_CPU_CALL_INDIRECT(ctx, base, ctr)`: the title's function table in a title, a recorded handler in the tests |
| `mfmsr` | X, opcode 31 xo 83 | a full fence, then rD = `ANY360_CPU_CHECK_GLOBAL_LOCK()` (0x8000 when the runtime's global lock is free, else 0) |
| `mtmsrd rS,1` | X, opcode 31 xo 178, L = 1 | a full fence, msr's bits 0x8020 from rS, then `ANY360_CPU_ENTER_GLOBAL_LOCK()` when rS is r13, else `ANY360_CPU_LEAVE_GLOBAL_LOCK()` |
| `mftb`, `mftbu` | XFX, opcode 31 xo 371, TBR 268 / 269 | rD = `ANY360_CPU_QUERY_TIMEBASE()` (the guest's 50 MHz time base), its high word for `mftbu` |
| `tw`, `twi`, `td`, `tdi` (`twllei`, `twlgei`, `tdllei`, `tdlgei` ...) | X opcode 31 xo 4 / 68, D opcodes 3 / 2 | `ANY360_CPU_TRAP(ctx, base, 0)` when one of the TO field's conditions holds (signed <, >, ==, unsigned <, >; the low words for `tw` / `twi`), nothing for TO 0, always for TO 31; `twi 31,r0,imm` passes imm as the trap's type |
| `bctr` through a jump table | XL, opcode 19 xo 528 | `switch` on the table's index register, a `goto` to each entry's label, `__builtin_trap()` outside the table (see below) |
| other `bctr` | XL, opcode 19 xo 528 | a tail call: `ANY360_CPU_CALL_INDIRECT(ctx, base, ctr)`, then a return |
| `bnectr` (conditional `bcctr` without the count decrement) | XL, opcode 19 xo 528 | the condition of BO / BI, then the tail call above |
| `blrl` | XL, opcode 19 xo 16, LK 1 | the old lr kept, lr set, then `ANY360_CPU_CALL_INDIRECT(ctx, base, old lr)` |

Beside the instructions:

- Jump tables are found from the words before each `bctr`, back to the first unconditional branch, as the titles'
  recompilation finds them: the `mtctr` of the target; for tables of offsets the `add` of a base and the `rlwinm` that
  scales the offset; the indexed load (`lwzx`: a table of addresses; `lhzx` / `lbzx`: offsets from the base); the
  `slwi` that scales the index, followed back to the index register; the `lis` / `addi` (or `ori`) pairs of the base
  and of the table. The entry count is the index's bound check before the `bctr` (`cmplwi` / `cmpwi` index,n: n + 1;
  `clrlwi` index,n: 2^(32 - n)), the table read up to the first entry that is not a word-aligned address in the image
  at or after the function's start (none: a tail call through ctr). Over every `bctr` of Mirror's Edge (1,264) and
  Crackdown (1,319) this gives the recompilation's switch (same index register, same targets) or its tail call,
  except for the tables without a bound check (10 in each title), which are left out (the function is not
  translated): their entry count is a guess on both sides.
- A function whose jump table reaches past its listed end (the function tables come from the image's `.pdata`, which
  leaves out-of-line case blocks out) takes them in, as the recompilation does: it ends at the first unconditional
  branch after its last table target, never past the next listed function's start. Words in between that nothing
  branches to are translated as code no label reaches.
- Each `switch` case is a label of the function: a table entry outside it or on another function's start leaves the
  function out.

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

- `bctr` through a jump table with an index outside the table: `__builtin_trap()`, as the recompilation; the ISA
  jumps to whatever address the word past the table holds. The bound check before every table the translator takes
  keeps the index inside on the titles' paths.
- `fctid`, `fctidz` of 2^63: 0x8000000000000000 (the host's conversion out of range); the ISA saturates,
  0x7FFFFFFFFFFFFFFF.
- `sraw` of 0x80000000 by 32 to 63, `srad` of 2^63 by 64 to 127: CA 0 (the amount taken as 31 / 63, which loses no
  one bit); the ISA sets CA to the sign, 1.
- `fnmadd(s)`, `fnmsub(s)` of a NaN operand: the NaN negated (0xFFF8000000000000 from 0x7FF8000000000000); the ISA
  gives the NaN unchanged.
- `stwcx.` / `stdcx.` store whenever the word holds the value in `ctx.reserved`, reservation or not (no `lwarx` before
  it, or one to another address); the ISA stores only while the reservation holds.

Where the ISA leaves a result undefined, the translation gives the recompilation's, which the tests take as expected:
the quotient of a division by zero or of the most negative value by -1 (0), the high words of `divw`, `divwu`,
`mulhw`, `mulhwu`, `fctiw`, `fctiwz`, the bits of `mfocrf` outside its field (0). `fres` and `frsqrte` give the exact
result rounded to single, an estimate the ISA allows (the console's own estimate tables give other values).

Changing these makes the translation differ from what the titles run today: it is a decision for the in-game binding,
not for the translator alone.

## Tests

- `cpu_isa_tests`: each case of `layer/cpu/tests/isa_cases.txt` (120 cases, encoded words ending in `blr`; `import`
  lines make addresses import thunks for the cases) translated at build time, against the ISA's definition of the
  same instructions written by hand in the test (Power ISA 2.06 Book I; the runtime hooks' calls, in order, as the
  recorded handlers see them), on 4,096 register states each (491,520 in all), integer edge values included, and edge
  operands the forced registers do not reach put in the cases themselves (`li` of a zero or -1 divisor, of shift
  amounts 0 to 127); floating-point cases draw their operands from edge values (zeros, infinities, a NaN, denormals,
  values that round or overflow). Each floating-point instruction is the first one of a case, so that its flush-mode
  change is checked. A carry an instruction sets is read by the next one (or captured with `addze`), so that no carry
  goes unchecked. Every register and every byte of guest memory compared. A line `@<address> <words>` of the cases
  file is data (a jump table) the translator reads and the test writes into guest memory (`kCaseData` in the
  generated header): the `bctr` cases expect the ISA's jump to the address read from the table, against the
  translation's `switch` on the index.
- `cpu_e2e_tests`: the end-to-end set (300 of the 26,764 Mirror's Edge functions both versions take, spread over the image, and up
  to 16 more for each runtime hook, of the functions whose own code calls it: 348 functions, 16,060 instructions, 681
  with their callees), each run from 64 register states on two guest spaces that start with the
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
[perf/archive/2026-11-27-cpu-translator-slice2](perf/archive/2026-11-27-cpu-translator-slice2/),
[perf/archive/2026-11-27-cpu-runtime-hooks](perf/archive/2026-11-27-cpu-runtime-hooks/),
[perf/archive/2026-11-27-cpu-bctr](perf/archive/2026-11-27-cpu-bctr/) and
[perf/archive/2026-11-27-cpu-scalar-rest](perf/archive/2026-11-27-cpu-scalar-rest/) (NOTES.md), and indexed in
[perf/FUNCTIONS.md](perf/FUNCTIONS.md).
