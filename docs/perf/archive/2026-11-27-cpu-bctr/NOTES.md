# 2026-11-27: Any360's PowerPC translator, `bctr`

The batch after slice 2 that `bctr` blocked: the branches through the count register that are not calls. `bctr` was
the second blocker of whole functions in Mirror's Edge (1,565 calls of otherwise translated functions, 1,264
occurrences); the titles also use `bnectr` and `blrl` once each. Nothing is bound in a title yet; no instruction id is
marked native (docs/CPU.md, "Done").

Machine: cores 2-3 only (`start /affinity C`, other batch sessions on the other cores), clang 22.1 -O2
(RelWithDebInfo), Windows 11.

## What is translated

Measured on the same base (main at dd25509, slice 2) with and without this batch; the other batches running beside
it (runtime hooks, scalar rest, VMX128) are not in these numbers.

| | slice 2 | + tail calls through ctr, switch tables | + function extents (case blocks past `.pdata`) |
|---|---:|---:|---:|
| Mirror's Edge functions (of 53,832) | 15,644 (29.1%) | 16,568 (30.8%) | 16,677 (31.0%) |
| Mirror's Edge instructions in them | 13.9% | 14.3% | 14.5% |
| Crackdown functions (of 41,219) | 11,567 (28.1%) | 12,431 (30.2%) | 12,528 (30.4%) |
| Crackdown instructions in them | 10.6% | 11.3% | 11.5% |
| functions with a `switch` translated (Mirror's Edge / Crackdown) | 0 / 0 | 0 / 0 | 17 / 21 |
| mnemonics covered (of the 352 the titles use) | 120 | 123 | 123 |

After the rebase on the runtime hooks batch (93f79d4: imports, `mfmsr` / `mtmsrd`, `mftb`, traps), both together:
Mirror's Edge 17,321 (32.2%, 15.3% of the instructions), Crackdown 13,138 (31.9%, 12.3%) (`share.txt`); the ISA cases
(74, 303,104 states) and the standard e2e set (320 functions, 10,334 instructions; 20,480 runs) all match again.

The gain is mostly the tail calls through ctr (`mtctr; bctr`: thunks and virtual tail calls, 781 Mirror's Edge
functions with one translate); switch tables alone gave no function, because the function tables (from `.pdata`) end
most switch functions before their out-of-line case blocks (168 of the 270 Mirror's Edge functions with a table), and
the rest are held by other instructions (`fcfid`, `fsqrts`, `fsel`, the vector unit) or callees. The function extents then
bring in 17 / 21 switch functions and their callers.

What blocks the most calls now (Mirror's Edge): `mfmsr` (1,630), `lbzu` (1,384), calls to imports (1,135), branches
out of the function to anywhere but a function start (904), `fcfid` (789), functions that run into the next one (707),
the vector unit (472), `subfe` (433), `divw` (406), `twi` (373). `bctr` is no longer in the list.

## Jump tables

The detector follows ReXGlue's (`detectJumpTable` and `scanForBounds` in its `codegen/function_scanner.cpp`) from the
XEX's words: the scan back from `bctr` to the first unconditional branch, the `mtctr`, the `add` / `rlwinm` chain of
offset tables, the indexed load (`lwzx`, `lhzx`, `lbzx`), the `slwi` of the index, the `lis` / `addi` / `ori` pairs,
the bound check (`cmplwi` / `cmpwi` n: n + 1 entries; `clrlwi` n: 2^(32 - n)), the entries read up to the first that is
not a word-aligned address in the image at or after the function's start.

Checked against the recompiled code over every `bctr` of both titles (each `// bctr` of the generated code: its
`switch` with index register and labels, or its `REX_CALL_INDIRECT_FUNC`), in function order:

| | Mirror's Edge | Crackdown |
|---|---:|---:|
| `bctr` compared | 1,236 | 1,269 |
| tail call, same | 828 | 501 |
| switch, same index register and same targets | 398 | 758 |
| switch left out (no bound check) | 10 | 10 |
| different | 0 | 0 |
| functions whose recompiled body has more `bctr` than their `.pdata` range (not compared) | 6 | 13 |

The tables without a bound check are left out (the function is not translated): their entry count is read until an
entry fails, a guess on both sides, and in two of them (the same code in both titles, `0x82AC7F5C` / `0x827999CC`)
the recompilation has 17 entries where reading on gives 25 / 24.

## Choices

- `switch` on the index register, as the recompilation: the e2e test's guest memory is made-up data, not the image,
  so a `switch` on the loaded target (ctr) would differ from the reference on every run; with the image's table both
  are the same jump.
- Outside the table, `__builtin_trap()` as the recompilation (docs/CPU.md lists it with the other differences from
  the ISA, which jumps to the word past the table). Every table the translator takes has a bound check before it; a
  case that reached the default could not be compared with the reference, which traps too.
- A table entry outside the function or on another function's start leaves the function out (none of the compared
  tables has one in a function that otherwise translates; the recompilation would call that function).
- Function extents follow the recompilation's rule (`discoverBlocks`: a table target past the function's end extends
  it), narrowed: from the last such target to the first unconditional branch, never past the next listed start.
  Checked on every function so extended (`extent_check.txt`): the labels are the recompilation's in all of them whose
  body translates (74 Mirror's Edge, 125 Crackdown; 18 / 32 others hold an instruction not covered). The recompilation emits only the blocks it reaches, the translation every word of the
  range: words between that no branch reaches are code no label enters.
- `bnectr` and `blrl`: as the recompilation's builders (`build_bnectr`, `build_blrl`). No function both tools take has
  either (each sits in a function with `mfcr`): they are checked by their ISA cases only. Other conditional `bcctr`
  forms (and `bcctrl`) are not used by the titles and are left out.

## Tests

- `cpu_isa_tests.txt`: 67 cases (5 new), 274,432 states, all match the ISA. A cases file line `@<address> <words>` is
  data the translator reads and the test writes into guest memory: `bctr_words` (a table of 4 addresses, `cmplwi` /
  `bgt` bound, `slwi` index), `bctr_bytes` (a byte table of word offsets from a base: `lbzx`, `slwi`, `add`),
  `bctr_tail`, `bnectr`, `blrl`. The expectations jump where the ISA does (the address read from the table), the
  translation's `switch` on the index.
- `cpu_e2e_tests.txt`: the standard set regenerated (Mirror's Edge, 300 functions of the 13,001 both versions take,
  11,251 instructions, 415 with their callees): 19,200 runs, all match. Taken every k-th, it has 8 tail calls through
  ctr and no `switch`.
- `cpu_e2e_bctr_tests.txt`: a set of every function both versions take whose translation (callees included) has a
  `bctr` (`cpu_e2e_bctr.py`: `tools/cpu_e2e.py` with that pick and `--min 2`, run once in the build folder; the suite
  keeps the standard set): 1,029 functions (19,712 instructions, 1,103 with their callees), 48 `switch` (808 cases),
  811 tail calls through ctr; 65,856 runs, 16,241,956 indirect calls, all match the reference.
- `reach_bctr.txt` (`reach_bctr.py`: a counter before each `switch` and ctr tail call of that set): 40 of the 48
  switches reached (185 different cases taken), 785 of the 811 tail calls. The 8 switches and 26 tail calls no state
  reaches are behind branches the made-up data does not take: for the e2e coverage batch (states aimed at missed
  blocks).

## Mutation check (`mutants_bctr.txt`, `mutants_bctr.py`)

15 mutants of the `bctr` set's translated header, drawn among what the states reach (`reach_bctr.txt`): 5 with two
cases of a reached switch swapped (one of them a case taken), 5 with the switch's index register changed, 5 with a
tail call through ctr + 4. All 15 caught. A first campaign reported every mutant missed: it ran the test through
`cmd /c start /affinity /wait`, which does not pass the test's exit code back; the script now reads the test's
output, and checks first that the unmutated header reports all match.

## Benchmark (`cpu_e2e_bench 5`, three runs a set)

Each function of a set, fastest of 5, up to 16 states x 64 rounds; the versions without steps; every timed run from
the same memory. Cores 2-3 while other sessions build and bench on the other cores: one function's speed moves
between 0.6x and 1.5x from one run to the next, so the per-function best of the three runs is the figure.

| set | functions timed | Any360 ns (sum of a call of each) | ReXGlue ns | speed | geometric mean of the speeds | B / allocations a call |
|---|---:|---:|---:|---:|---:|---|
| baseline: empty function, same context copy and call | | 7.6 - 14.0 | | | | 0 / 0 |
| standard, run 1 | 298 | 27,209.9 | 27,320.8 | 1.00x | 0.999x | 0 / 0 |
| standard, run 2 | 298 | 30,692.8 | 31,951.6 | 1.04x | 0.997x | 0 / 0 |
| standard, run 3 | 298 | 35,463.2 | 37,101.8 | 1.05x | 0.999x | 0 / 0 |
| standard, best of 3 per function | 298 | 26,256.2 | 26,374.8 | 1.00x | 0.999x | 0 / 0 |
| `bctr` set, run 1 | 1,026 | 117,124.6 | 126,228.3 | 1.08x | 0.997x | 0 / 0 |
| `bctr` set, run 2 | 1,026 | 123,246.2 | 116,261.7 | 0.94x | 1.006x | 0 / 0 |
| `bctr` set, run 3 | 1,026 | 94,249.1 | 93,901.5 | 1.00x | 1.002x | 0 / 0 |
| `bctr` set, best of 3 per function | 1,026 | 92,091.8 | 92,102.0 | 1.00x | 1.001x | 0 / 0 |
| of which with a `switch` | 43 | 917.4 | 900.0 | 0.98x | 0.994x | 0 / 0 |
| of which with a tail call through ctr | 783 | 11,598.7 | 11,846.2 | 1.02x | 1.000x | 0 / 0 |

Parity: the `switch` and the tail call are the recompilation's statements (the same `switch` on the same register,
`__builtin_trap()` default, the same indirect call and return). The functions with a `switch` sum 17 ns more over 43
functions; no function among them is below 0.95x in more than one run but `sub_826A7E68` (0.97x, 0.73x, 0.72x), whose
two switches no state reaches: the gap is code placement or the run's noise, not the translated `bctr`. The assembly
was not compared.

## Next

- e2e coverage: the 8 switches and 26 ctr tail calls of the `bctr` set no made-up state reaches.
- The function tables from `.pdata`: other functions run past their listed end without a jump table (707 calls
  blocked in Mirror's Edge, "functions that run into the next one") or branch into the middle of another (904); the
  recompilation's block discovery covers both and would replace the narrow extent rule here.
- Binding: `ANY360_CPU_CALL_INDIRECT` needs the title's function table (it now also takes the tail calls through
  ctr and `blrl`).
