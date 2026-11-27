# 2026-11-27: Any360's PowerPC translator, the scalar rest

The scalar instructions slice 2 left out, most used first (`db/instructions.json`; the board's batch "scalar-rest"):
the update forms of the loads and stores, the algebraic, indexed doubleword, byte-reversed and reservation forms, the
carrying adds and subtracts, the high multiplies and divides, the 64-bit and register shifts, the condition register
moves and logic, the floating-point conversions, square roots, select and estimates, and the cache-zero and hint forms.
107 mnemonics (243 covered in all with the runtime hooks and `bctr` batches). Nothing is bound in a title yet; no instruction id is marked native (docs/CPU.md,
"Done").

Machine: cores 4-5 (`start /affinity 30`, `ninja -j 2`), clang 22.1 -O2 (RelWithDebInfo), Windows 11. Other batch
sessions shared the machine on their own cores.

## What is translated

A title runs whole functions, so the share that counts is theirs (a function translated with all its callees; calls
to imports count as translated, `share-scripts/share_imports.py`). Before: `origin/main` at the rebase, with the
runtime hooks and `bctr` batches. Alone on slice 2 the batch took Mirror's Edge from 29.1% to 32.4% of its functions
(13.9% to 20.6% of its instructions) and Crackdown from 28.1% to 31.1%.

| | before (runtime hooks + `bctr`) | after |
|---|---:|---:|
| Mirror's Edge functions | 17,321 of 53,832 (32.2%) | 30,907 (57.4%) |
| Mirror's Edge instructions in them | 15.3% | 32.6% |
| Crackdown functions | 13,138 of 41,219 (31.9%) | 14,832 (36.0%) |
| Crackdown instructions in them | 12.3% | 17.4% |
| mnemonics covered | 128 | 243 |
| their static occurrences in the titles | 97.0% | 97.6% |

The three batches together give more than their sum: most of Mirror's Edge's lock-holding code needed `mfmsr` /
`mtmsrd` (runtime hooks) and `lwarx` / `stwcx.` (this batch) at once, and the functions that call it everything
else. The instruction share grows more than the function count on Crackdown: the new functions are the long ones
(conversions and square roots in maths code, divides, 64-bit shifts, carry chains).

What blocks the most calls from otherwise translated functions now (Mirror's Edge, `share-scripts/callees_imports.py`):
functions that run into the next one (2,998), VMX128 (1,115), branches into another function (648), jump tables whose
targets are not labels of the function (219), words that are not instructions (154), `mffs` (27). `dcbtst` (a cache
hint) blocked 1,657 calls before it was added, nearly all of them calls to one block copy (sub_829E37C0) that then
fails as a function running into the next one: the count moved to that line.

Left out of this batch: `mffs` / `mtfsf` (5 each a title; the context's floating-point status word has no guest
image yet, `Fpscr` keeps only the flush bit), `mfxer` (no title uses it; ReXGlue's statement shifts an 8-bit field
into the sign of an int), `lmw` / `stmw` (no title uses them), the record and overflow forms (`OE`) of the XO
instructions (left as unsupported words), the aliases ReXGlue cannot recompile (`crset`, `crclr`, `crnot`, `crmove`;
no title uses them).

## How the decoder was checked

- Word against name (`crosscheck` in the session's scratch, not kept): every instruction word of every recompiled
  function of Mirror's Edge (53,832 functions) and Crackdown (41,219), paired with ReXGlue's comment naming it (the
  comments resynchronised at each label, and a function dropped if a fixed-opcode mnemonic disagrees with its word: 0
  dropped). 0 words decoded whose mnemonic is not in `COVERED_MNEMONICS`, 0 words of a covered mnemonic left
  undecoded. It found three names slice 2 already decoded but did not list: `or r1,r1,r1` (`cctpl`), `or r2,r2,r2`
  (`cctpm`), `or r31,r31,r31` (`db16cyc`), the thread priority hints ReXGlue writes nothing for; they are now listed
  and translated to nothing (a case checks it), so that `cpu_e2e.py` does not stop once a function with one of them
  joins the set.
- The statements follow ReXGlue's builders (`src/codegen/builders/`: `memory.cpp`, `arithmetic.cpp`, `logical.cpp`,
  `floating_point.cpp`, `system.cpp`, `helpers.h`) operation for operation, written on `ppc_model.h`'s context
  (`Load32` ... for `REX_LOAD_U32`, `Raw` for `REX_RAW_ADDR`, `std::byteswap` for `__builtin_bswap32`, scoped locals
  for its `temp`). The carry forms compute the same low-word carries with the operands read once.
- `Ctx` gained `reserved` (the recompiled context's reservation register): `lwarx` / `ldarx` keep the word as it lies
  in memory there, `stwcx.` / `stdcx.` compare-and-swap it. The tests draw it at random and compare it.

## Tests

- `cpu_isa_tests.txt` (after the rebase on the runtime hooks and `bctr`): 120 cases (46 of this batch), 491,520
  states, all match the ISA. Edge operands are put in the cases
  themselves (`li` of 0 and -1 divisors, shift amounts 0, 1, 31, 32, 63, 64 and 127, the most negative doubleword
  built with `rldicr`), since the harness forces edge values into r4 only. Each floating-point mnemonic is the first
  floating-point instruction of its own case (its flush-mode change compared), with operand sets of its own:
  integer bits for `fcfid` / `stfiwx`; values around the conversion limits and halves for `fctiw(z)` / `fctid(z)`;
  zeros, infinity, a NaN, denormals and positive values for the roots and estimates; signs, zeros and a NaN for
  `fsel`'s frA.
- Two holes found by mutating the translator by hand (`sraw`'s clamp, `fctiwz`'s comparison, `subfze`'s carry,
  `divwu` by zero, the flush flag of the floating-point update forms, `mfocrf`'s field), closed before the archive:
  `subfze`'s carry was overwritten by the next instruction before anything read it, and so were the carries of
  `sraw`, `srad` and `sradi` but the last. The carry case now chains every carry into the next instruction that reads
  it, and the shift cases capture each carry with `addze`. All six mutants are then caught but `fctiwz`'s `>=` to
  `>`, which is equivalent (the host's conversion of 2147483647.0 gives INT_MAX either way).
- `fnmsubs` (slice 2) drew a NaN operand and expected it negated, which is the recompilation's result and not the ISA's
  (the ISA returns a NaN operand unchanged): it now draws without the NaN, as the new `fnmadd`, `fnmadds`, `fnmsub`,
  and the NaN is checked by name below.
- Known differences from the ISA, each checked by name in `cpu_isa_tests` (the translation follows the recompilation,
  which the e2e test and the in-game check are defined against):
  - `fctidz` and `fctid` of 2^63: 0x8000000000000000; the ISA saturates to 0x7FFFFFFFFFFFFFFF. The builder tests
    `> double(LLONG_MAX)`, and double(LLONG_MAX) is 2^63 itself, so 2^63 reaches the host's conversion, which gives
    its out-of-range value.
  - `sraw` of 0x80000000 by 32: CA 0; `srad` of 2^63 by 64: CA 0; the ISA sets CA to the sign (1). The amount is
    clamped to 31 / 63 and CA computed from the bits lost by that shift, which are all zero for the most negative
    value.
  - `fnmadds` of a NaN frA 0x7FF8000000000000: 0xFFF8000000000000 (`-std::fma` negates the NaN); the ISA gives the NaN.
  - `fsqrt` of -1: 0xFFF8000000000000, the host's default NaN (the same difference as slice 2's `fadd` inf + -inf).
  - `stwcx.` with no `lwarx` before it, the word equal to `ctx.reserved`: stores and sets eq; the ISA does not store
    without a reservation. The recompilation keeps no reservation address, only the value.
- Where the ISA leaves the result undefined, the tests expect the recompilation's: 0 for a division by zero and for
  the most negative value by -1, the high words of `divw` / `divwu` (0), `mulhw` / `mulhwu` (sign / zero extension),
  `fctiw` / `fctiwz` (the low word's sign, 0 for a NaN), the bits of `mfocrf` outside its field (0). `fres` and
  `frsqrte` give the exact result rounded to single, an estimate within the ISA's bounds; the console's estimate tables
  give other bits, which only the in-game check can weigh.
- `cpu_e2e_tests.txt` (after the rebase): Mirror's Edge, 348 functions (300 spread over the image and 48 for the
  runtime hooks; 16,060 instructions, 681 with their callees) of the 26,764 both versions take (14,134 with this batch
  alone, 12,386 before it), 22,272 runs, 2,069 stopped at their bound, 10,941,170 indirect calls, 2,192,878 import calls,
  357,222 global lock checks / enters / leaves, 424 traps: all match the reference, registers, memory and hook calls.
  The functions that hold the global lock now enter the set: the lock sections' `lwarx` / `stwcx.` were what kept
  them out. Before the rebase (this batch on slice 2): 300 functions, 14,645 instructions, 19,200 runs, all match.
  The 300 functions hold 22 of the 107 new mnemonics (`fcfid` 54, `fsqrts` 70,
  `fctiwz` 30, `fsel` 27, `fsqrt` 21, `divw`, `divwu`, `subfe` ...).
- Aimed e2e sets (made locally, not kept: up to 12 functions a new mnemonic, all both versions take): Mirror's Edge
  265 functions (28,564 instructions, 16,960 runs) and Crackdown 239 functions (18,179 instructions, 15,296 runs): all
  match. Together they reach 49 of the 106 mnemonics then new (`dcbtst` / `dcbst` came
  after). The other 57 have no function both tools take in these two titles: the functions that hold them have
  something else left out (an instruction of another batch, a callee) or ReXGlue wrote the access as memory-mapped
  I/O (`REX_MM_`, which `tools/ppc_model.py` does not model: most byte-reversed accesses and some X-form update
  stores, where the base came from an I/O address). Those (`addc`, `adde`, `addme`, `subfme`, `subfze`, `divd`, `mulhd`,
  `mulhdu`, `andis.`, `rlwnm`, `rldic`, `fctiw`, `fres`, `fnmadd`, the reservation atomics, `sthbrx`,
  the CR moves and logic, `mtxer`, the priority hints, and the update forms but `lbzu`, `lwzu`, `ldu`, `stbu`,
  `stwux`) rest on the ISA cases alone until a title binds them.
- `mutants.txt` (`mutants.py <build> 20 2`, on the set before the rebase): 20 mutants of the e2e header, each a constant (decimal or hexadecimal)
  + 1 in a statement only this batch's instructions write (154 statements in the header, 72 constants; a statement
  another instruction writes the same, such as a record form's cr0 compare, is left out, which a first run that drew
  mostly those showed). 3 caught (`srd`'s amount mask twice, `fctiwz`'s NaN value once). The 17 missed all change a
  value used only on an edge input: the NaN result of `fctiw(z)` / `fctid(z)` (10), the result of a division by zero
  or of the most negative value by -1 (6), the result of `sld` by 64 or more (1). `reach.txt` (`reach.py`, an abort before the line): of four
  missed lines, the `fctiwz` one is reached by no state; the `divwu` and `sld` ones are reached, never with a zero
  divisor or an amount of 64 or more; the `divw` one did not build with the abort placed there. The made-up states rarely put
  a NaN, a zero divisor or such an amount just there (one state did for the one `fctiwz` caught), and the titles' code
  often tests a divisor before dividing. Every one of these values is compared by the ISA cases, which caught the same
  kinds of mutants made in the translator by hand (`divwu` by zero, `fctiwz`, the shift clamps). The e2e set is the
  weaker check for this batch: its instructions do their work on edge values that random data does not reach.

## Benchmark (`cpu_e2e_bench_run1-3.txt`, `cpu_e2e_bench 5`)

After the rebase: the 348 functions (342 timed: for 6, no state ends within the bounds), up to 16 states each, 64
rounds, fastest of 5; the versions without steps. Each timed run starts from the same memory. The other batch
sessions were building and benching on their own cores during the runs.

| run | Any360 ns (sum of a call of each) | ReXGlue ns | speed | geometric mean of the 342 speeds | B / allocations a call |
|---|---:|---:|---:|---:|---|
| baseline: empty function, same context copy and call | 9.4 - 10.2 | | | | 0 / 0 |
| 1 | 64,615.5 | 68,304.6 | 1.06x | 0.986x | 0 / 0 |
| 2 | 82,512.9 | 79,370.3 | 0.96x | 0.998x | 0 / 0 |
| 3 | 59,569.5 | 60,468.5 | 1.02x | 0.988x | 0 / 0 |
| best of 3 per function | 52,284.7 | 55,292.2 | 1.06x | 0.991x | 0 / 0 |

Parity within the noise of a shared machine: the totals move by 40% between runs, and 73 functions sit below 0.95x
against 54 above 1.05x in the best of 3. The 43 functions of the set that hold an instruction of this batch: 1.008x
(geometric mean of the best of 3), slowest 0.824x.

Before the rebase (this batch on slice 2, 300 functions, 14,645 instructions, a quieter machine): best of 3 per
function 78,164.1 ns against 78,121.4 ns, 1.00x, geometric mean 1.002x; the 34 functions holding a new instruction
1.001x, slowest 0.979x; 0 B / 0 allocations. Those runs are not kept (the archive holds the runs of the pushed set).

The totals are not comparable with slice 2's: the set is other functions, and the context is larger (`reserved`,
`msr`).

## Choices

- The recompilation's statements where they depart from the ISA (the five above), as in slices 1 and 2: the e2e test,
  the parity bench and the in-game check are defined against the recompilation, and the titles run it today.
  Console-exact against recompilation-exact is decided at the in-game binding (docs/CPU.md).
- Update forms write only rA's low word, as `stwu` since slice 1 and ReXGlue's update builders; the forms the ISA
  calls invalid (rA 0, and rA = rD for loads) are left out.
- Byte-reversed and X-form stores are always plain memory accesses: the translator does not yet follow
  memory-mapped I/O bases, which binding needs for every load and store (docs/CPU.md, "Done").
- `scalar_rest` is its own method, tried before the decoder's other branches: the batches that edit `decode` in
  parallel (runtime hooks, `bctr`, VMX128) meet this one in a call and a table block, not inside its branches.

## Next

By the calls they block: functions that run into the next one (one block copy behind most of them), `bctr`, `mfmsr` /
`mtmsrd`, imports, the vector unit; then the e2e coverage of the mnemonics no function both tools take reaches.
