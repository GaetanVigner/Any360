# 2026-11-26: Any360's PowerPC translator, first slice

The roadmap (docs/ROADMAP.md) puts the CPU instructions first: 10.2 M static occurrences in the known titles, every id
`reference` (ReXGlue). This slice is Any360's own translator for the 52 most used mnemonics (86.7% of the occurrences; whole
functions are less: 12.1% of Mirror's Edge's, 2.4% of its instructions, and 11.2% / 1.3% of Crackdown's),
from the XEX image's words, with its tests and its benchmark. Nothing is bound in a title yet; no instruction id is
marked native (docs/CPU.md, "Done").

Machine: cores 0-7 (`start /affinity FF`), clang 22.1 -O2 (RelWithDebInfo), Windows 11.

## Tests

- `cpu_isa_tests.txt`: 27 cases (every mnemonic of `COVERED_MNEMONICS` has one; `ppc_translate.py --cases` stops the
  build otherwise), 110,592 states, all match the ISA.
- `cpu_e2e_tests.txt`: Mirror's Edge, 300 functions (5,122 instructions; 346 with their callees) of the 4,411 both
  versions take, 19,200 runs, 71 stopped at the 16,384-step bound (in both versions at the same step): all match the
  reference, registers and memory.
- Floating-point flush mode: the translation sets MXCSR to keep denormals before the first floating-point instruction
  after a label or a call, as the titles' recompilation does. Without it (checked by removing the calls), 26 functions
  of the e2e set differ: states start with flush on or off.
- Count register: `ctr` is 64-bit, decremented whole and tested on its low word, as the recompilation does; the model
  (`ppc_model.h`) carries it, so `bdnz` and the other ctr forms are in the e2e set.
- `mutants.txt` (`mutants.py`): 20 mutants of the translated header, one integer constant + 1 each, rebuilt and run:
  14 caught. The 6 missed are all on lines no state reaches (`reach.py`: an `abort()` inserted before each line, the
  test still passes): a path coverage gap of the made-up states, not a difference the comparison lets through. A
  first campaign before the flush and ctr fixes caught 17 of 20 on another draw (one equivalent mutant, two values the
  data did not reach); that made the data give 0-7, 1 and -1 more often (`cpu_space.h`).

## Benchmark (`cpu_e2e_bench_run1-3.txt`, `cpu_e2e_bench 5`)

The 300 functions, up to 16 states each, 64 rounds, fastest of 5; the versions without steps. Each timed run starts
from the same memory (`Space::Refill`, pages already committed by an untimed run), and the states kept are those whose
every call of the 64 rounds ends within the step bound. The first version of the bench reused the memory of earlier
rounds unchecked: one run never ended (a loop over pointers an earlier round had overwritten), and its times were of
drifting memory; those results are replaced.

| run | Any360 ns (sum of a call of each) | ReXGlue ns | speed | geometric mean of the 300 speeds | B / allocations a call |
|---|---:|---:|---:|---:|---|
| baseline: empty function, same context copy and call | 7.9 - 8.8 | | | | 0 / 0 |
| 1 | 6,103.5 | 6,042.9 | 0.99x | 1.000x | 0 / 0 |
| 2 | 6,268.7 | 6,228.5 | 0.99x | 0.997x | 0 / 0 |
| 3 | 6,343.2 | 6,296.3 | 0.99x | 1.004x | 0 / 0 |
| best of 3 per function | 6,062.7 | 6,012.3 | 0.99x | 0.999x | 0 / 0 |

Parity: the translation is the same statements as ReXGlue's for these instructions. 2 functions below 0.95x
(sub_826DD9B8 0.94x, sub_82A7EE80 0.95x: their statements are identical in both versions, line for line), 1 above
1.05x; all within run-to-run spread. The sums include the baseline's ~8 ns a call (context copy and call).

## Choices

- Ranked by static occurrences: no per-instruction profile exists; the roadmap says so in its source column.
- The e2e oracle is ReXGlue's recompilation through `tools/ppc_model.py` on the same context: the same functions run
  from the same states, compared byte for byte. Functions the model cannot carry (indirect calls, imports) stay out of the e2e set; their instructions are checked by `cpu_isa_tests` against the ISA.
- Record-form compares (`.`) set cr0 from the low word, as the titles' recompilation does; `stwu` writes back rA's low
  word. Both match the titles' 32-bit addressing; register states keep the upper word zero or sign-extended, as the
  titles' code leaves them.
- Bounded runs instead of curated inputs: made-up pointers make some loops (`begin != end` with a stride) run to the
  end of the address space. The step check is in the generated headers only for the tests; the bench times copies
  without it (`cpu_e2e_plain_*.h`).
- The mnemonic `a` (1 occurrence in The Simpsons Game) was a hand patch's comment counted as an instruction:
  `tools/coverage.py` now takes an instruction comment only when its operands are one comma-separated word.

## Next

The next mnemonics by use (`lbz`, `bctrl`, `stb`, `lwzx`, `lvx128`, `subf`, `ori`, `fmr`, `lfd`, `fsubs`, `fmadds`,
`fcmpu`, `fadds`, `lhz`, `sth`) raise the share of whole functions translated; `bctrl` needs the title's function
table. The e2e set needs a measure of the blocks its states reach, and states aimed at the blocks they miss (the
mutation campaign's 6 missed mutants). Then a title binds translated functions and checks them in game.
