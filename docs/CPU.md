# CPU: Any360's PowerPC translator

The titles' code runs today as ReXGlue recompiled it: every CPU instruction id is `reference` on the progress page,
which counts as not migrated (CLAUDE.md). Any360's own translator reads a title's functions from the PowerPC words of
its XEX image and writes them as C++ on `any360/xdk/ppc_model.h`'s context (`Ctx`: the registers; `Memory`: the guest
memory), with ReXGlue's recompilation kept only as the oracle its output is compared with.

- `tools/ppc_translate.py`: the decoder and emitter. `--image` / `--functions` translate functions of a title (with
  their callees); `--cases` translates the instruction test cases.
- `tools/cpu_e2e.py`: the end-to-end set: a title's functions both versions take, translated by Any360 and recompiled
  by ReXGlue (`tools/ppc_model.py`), written into the build folder (nothing of the title is kept in the repository).
- `layer/cpu`: the tests and the benchmark, built with `layer/sys` and run by `layer\sys\test.ps1`.

## Instructions covered

The first slice is the most used instructions of the known titles (docs/ROADMAP.md, static occurrences): 52 of the 352
mnemonics the titles use, 86.7% of their 10.2 M static occurrences.

| Instructions | Encoding | Translation |
|---|---|---|
| `addi`, `li` | D, opcode 14 | `rD = rA + SIMM` (64-bit), `rD = SIMM` when rA is 0 |
| `addis`, `lis` | D, opcode 15 | `rD = rA + (SIMM << 16)` (64-bit) |
| `lwz`, `stw`, `stwu` | D, opcodes 32, 36, 37 | big-endian 32-bit load (zero-extended) / store; `stwu` writes the address back into rA's low word |
| `ld`, `std` | DS, opcodes 58 / 62 (sub-opcode 0) | big-endian 64-bit load / store |
| `lfs`, `stfs`, `fmuls` | D opcodes 48 / 52, A opcode 59 xo 25 | single to double on load, double to single on store, product rounded to single |
| `cmpwi`, `cmplwi`, `cmpdi`, `cmpldi`, `cmpw`, `cmplw`, `cmpd`, `cmpld` | D opcodes 11 / 10, X opcode 31 xo 0 / 32 | the field's lt / gt / eq, so from XER |
| `rlwinm` (`clrlwi`, `rotlwi`, `slwi`, `srwi` ...) | M, opcode 21 | ISA-exact: the low word rotated in a doubled 64-bit value, masked by MASK(mb + 32, me + 32) |
| `or`, `mr`, `add` (and `.` forms) | X / XO, opcode 31 xo 444 / 266 | 64-bit; the `.` forms set cr0 from the low word as a signed compare with 0 |
| `mflr`, `mtlr`, `mfctr`, `mtctr` | XFX, opcode 31 xo 339 / 467 | lr / ctr |
| `b`, `bl` | I, opcode 18 | a jump inside the function; `bl` sets lr and calls the callee's translation |
| `bc` forms (`beq`, `bne`, `blt` ..., `bdnz`, `bdz`, `bdnzf`, `bdnzt`, `bdzf` ...) | B, opcode 16 | the condition of BO / BI; when BO asks, ctr (64-bit) decremented first and its low word tested |
| `bclr` forms (`blr`, `beqlr` ..., `bdzlr`) | XL, opcode 19 xo 16 | a return, conditional as above |

The floating-point flush mode follows the titles' recompilation: before the first floating-point instruction of a
block (after a label or a call), the translation sets the host's MXCSR to keep denormals (`fpscr.disableFlushMode()`),
as ReXGlue's code does; the tests start states with flush on and off.

`tools/ppc_translate.py --cases` stops the build when a mnemonic of `COVERED_MNEMONICS` (the list the roadmap marks
`translated`) has no case in `isa_cases.txt`: no mnemonic is counted as translated without its ISA test.

A function is translated only when all its instructions are covered, it has no indirect branch (`bctr`, `bctrl`) and
no branch out of it but calls, and every callee can be translated. So far that is 12.1% of Mirror's Edge's functions
(6,514 of 53,832; 2.4% of its instructions) and 11.2% of Crackdown's (4,631 of 41,219; 1.3%). The next instructions by
use are `lbz`, `bctrl`, `stb`, `lwzx`, `lvx128`, `subf`, `ori`, `fmr`, `lfd`, `fsubs`, `fmadds`, `fcmpu`, `fadds`,
`lhz`, `sth` (docs/ROADMAP.md).

## Tests

- `cpu_isa_tests`: each case (27, so 110,592 states) of `layer/cpu/tests/isa_cases.txt` (encoded words ending in `blr`) translated at build
  time, against the ISA's definition of the same instructions written by hand in the test (Power ISA 2.06 Book I), on
  4,096 register states each, edge values included; every register and every byte of guest memory compared.
- `cpu_e2e_tests`: the end-to-end set (300 of the 4,411 Mirror's Edge functions both versions take, spread over the
  image: 5,122 instructions, 346 functions with their callees), each run from 64 register states on two guest spaces that start with the
  same memory; every register and every byte either version touched must match. Runs on made-up data are bounded: both
  versions pass `ANY360_CPU_STEP()` at each function entry and label, and a run that passes 16,384 of them stops there,
  in both at the same step. Built only where the title's files are (`titles/<title>/pipeline.json`: `image`,
  `generated`; `docs/data/functions-<title>.json`); `ANY360_CPU_E2E_TITLE` picks the title.
- `cpu_e2e_bench`: each function of the set, Any360's translation against ReXGlue's recompilation, without the steps:
  64 rounds over the states on one memory, every timed run starting from the same memory, on the states whose every
  call of the rounds ends within the steps; ns a call and host allocations a call.

## Done

An instruction id turns `native` only when Any360's translation of it runs in a title: the title's functions bound to
their translations, checked in game (0 different) and measured there. Until then the roadmap shows it as `translated
(not bound)`.

## Measures

The results are in [perf/archive/2026-11-26-cpu-translator-slice1](perf/archive/2026-11-26-cpu-translator-slice1/)
(NOTES.md) and indexed in [perf/FUNCTIONS.md](perf/FUNCTIONS.md).
