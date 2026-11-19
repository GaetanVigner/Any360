# 2026-11-19: native leaves keep the guest registers in host registers (ppc_model.h, tools/ppc_model.py)

The owner asked for the reverb loop's vector form next. First I measured what the loop's compiled instructions are
spent on (clang's annotated assembly of the leaf; `chains.py` for the dependency chains).

The promoted loop (loc_82A4E774) compiled to about 1445 x86 instructions an iteration:
- 727 touched the stack, 350 of them annotated spills;
- 168 float/double conversions for 70 floating-point operations;
- 48 byte swaps (the accesses left in memory).

`chains.py` gives a longest path of 66 cycles in an iteration and a longest recurrence (a filter's state) of 22
cycles, against about 780 cycles measured an iteration. So the loop is bound by its instruction count, not by
latency, and floating point is a small part of it: a vector form would have worked on the wrong thing.

The LLVM IR showed why so many stack accesses: the leaf's local copy of the register context (`Ctx`, 1088 bytes)
survived as one stack object, so every guest register was a load or a store. Two things pinned it:
- `RestGprLr` (restoring the saved registers, 8 call sites) was not inlined, so the context's address escaped into
  a real call;
- the unaligned vector stores stvlx / stvrx index `ctx.vN.u8[i]` by a variable `i`, and an access at a variable
  offset keeps clang's SROA from splitting the context into registers.

## The change (generic: every leaf tools/ppc_model.py --native makes)

- any360/xdk/ppc_model.h: SaveGprLr, RestGprLr, SaveFpr and RestFpr always inlined, their loops fully unrolled
  (`first` is a constant at each call; the member-pointer table then resolves at compile time).
- tools/ppc_model.py: stvlx / stvrx loops read a copy of the vector register (`const VReg lanes = ctx.vN;`), so the
  context is only reached at constant offsets.

After it, the context's integer and floating-point registers are SSA values. Two small groups still sit on the stack
(the condition fields and 17 vector registers); they are rarely used in the hot loop. The promoted loop compiles to
932 instructions an iteration (1445 before), its 352 stack accesses now all spills (148 locals for 16 registers).

## Tests and bench

`xdk_xaudio_reverb_tests`: 681 checks, 0 failing (the same bits).

Bench (`xdk_xaudio_reverb_bench 5`, cores 8-15; `bench.txt`), microseconds a frame:

| | promoted loop | its loop plain | vs the model (86-91) |
|---|---:|---:|---:|
| before (e3f79c6) | 48.2 | 65.2 | 1.80x |
| context in registers | **27.2** | 38.1 | **3.2x** |

The loop is promoted on 10000 of 10000 frames; 0 B and 0 allocations a call.

## In game (Radeon, cores 8-15, the city route, records on)

Verify launch (`regs_verify_run.txt`, crackdown_240.log):
- 33164 reverb calls compared with the title's own code: 0 different.
- The loop was promoted on all 33164.

Profile (80 s, `profile_after.*`): sub_82A4F410 1005 ms of 80 s, about 12.6 ms/s of a core (33-42 in the previous
steps' profiles, 63 before them). It moves from the table's first rank to the seventh.

A/B, 10 v 10 launches of 150 s:
- First half after first (`ab_reverbregs_runs.txt`), second half before first (`ab_reverbregs2_runs.txt`).
- Merged in `ab_reverbregs_final_*`; per-launch periods in `periods_1pct_low.txt`.

| | before (e3f79c6) | after |
|---|---:|---:|
| FPS (steady periods, mean / median) | 62.7 / 62.6 | 62.6 / 62.4 |
| 10% low | 61.7 | 61.2 |
| 1% low (mean / median) | 59.7 / 60.3 | 57.8 / 60.0 |
| private memory (mean) | 2571 MB | 2575 MB |

Rank z: steady FPS -0.97, 10% low -1.48, 1% low -1.91. None reaches 1.96. The halves disagree:
- first half: 10% low -2.23, 1% low -2.83;
- second half: -0.21 and -0.12.

The per-launch periods show two kinds of launch, taken at random:
- Some stutter early (1% lows of 40 / 37 FPS in periods 3-4) and then run clean.
- The others start clean and later hit the records mode's stretches (periods at 51, then 34 FPS).

| | launches of the second kind |
|---|---:|
| after | 6 of 10 (242, 244, 246, 248, 250, 253) |
| before | 4 of 10 (243, 249, 254, 256) |

Within each kind, the dips are the same size on both sides. The change touches only the audio thread's reverb
(faster), not the frame's path. I read the gap as this mix, not an effect of the change. It is kept here so a later
pass can look again.

## Next

- The promoted loop's remaining cost is register pressure: 148 locals spill.
- Its floating point is a small share: 70 operations and 84 conversions of 932 instructions.
- A vector form across filters is not established (no repeated sequences found in the interleaved code).
