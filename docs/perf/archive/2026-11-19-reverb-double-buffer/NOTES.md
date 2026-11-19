# 2026-11-19: XAudio reverb's double-buffer words picked by a selector (tools/ppc_model.py --promote-memory)

Follows 2026-11-19-reverb-loop-memory. That step left 40 of the hot loop's 289 loads and stores in memory, 10 groups
of the same form. In each group, a filter reads one word at `state + k + 4 * (x & 1)`: a double buffer, its flag x
an invariant word of the state. It also reads and writes the two words that one can be, at `k` and `k + 4`. The
place guess gave the first word a spread of 4, so it and the two exact words it overlaps stayed in memory.

## The change (generic, tools/ppc_model.py)

- A word whose place has a spread that is a multiple of 4 is one of the exact words it covers, the same one for the
  whole loop, because its index is an invariant.
- Each of those words is held in a local (loaded at the check from its root when no access of its own names it).
- The access picks among them with a selector computed at the check. The selector is `q = address - (root + k)`,
  and the check requires `q <= spread` and `q % 4 == 0`. A load becomes `(q == 0 ? m_a : m_b)`; a store becomes an
  if/else over the candidates.
- If any candidate has to stay in memory for another reason, the spread word stays with it.

For the reverb's loop:
- All 241 accesses at invariant addresses are now in locals (201 before): 148 words, 10 picked by a selector.
- 40 accesses are left in memory (the delay lines and the stack's work buffers).
- 343 checks, down from 1033 (fewer accesses left in memory to keep apart).

## Tests

`xdk_xaudio_reverb_tests`: 681 checks, 0 failing.

The double-buffer flags were not covered before. `NewState` zeroed seven of the ten flags (they are integer words),
and the other three are delay taps. So the new optional argument `flags` draws each flag's bit 0
(`kFlags` in xdk_xaudio_reverb_state.h). The flags were found from the generated selectors: `k - 4 * C`, with C the
constant in `(x & 1) + C`.

| | selectors seen at 4 (of 312 frames) |
|---|---|
| without `flags` | 3 of 10 selectors, the three whose flags are taps |
| with `flags` (half the regular sequences, the forced plain ones) | all 10, 24-216 frames each |

A check that the test catches a wrong pick: the 10 selectors' arms swapped in a diagnostic copy of the header made
the very first frames differ from the model.

Drawing every integer word's bit 0 instead made the original code write past the state, both sides alike:
"written outside its plan" at state + 0x4B9D0. Those integers are positions, so only the flags are drawn.

## Bench (`xdk_xaudio_reverb_bench 5`, cores 8-15; `bench_alternated.txt`)

Microseconds a frame, the previous step's executable (mem) against this one (sel), alternating 4 x 5 runs:

| | us a frame | vs the model (85-92) |
|---|---:|---:|
| 2026-11-19-reverb-loop-memory (Any360 30084e1) | 49.5-50.5 | 1.73x |
| double-buffer selectors | 47.7-48.5 | 1.80x |

- The second pair's runs were slow across the whole process (the plain copy too: 86 against 65 us), the machine
  being busy; they are left out.
- The loop is promoted on 10000 of 10000 frames.
- 0 B and 0 allocations a call.
- The earlier ceiling without any check (44 us) is near.

## In game (Radeon, cores 8-15, the city route, records on)

Verify launch (`--any360_xdk_verify`, `sel_verify_run.txt`, crackdown_228.log):
- 33042 reverb calls compared with the title's own code: 0 different.
- The loop was promoted on all 33042, plain on 0.

Profile (80 s, `profile_after.*`): sub_82A4F410 33 ms/s of a core, against 42 in the previous step's after profile.
That launch caught less processor time in all (1.10 against 1.27 cores), and the reverb's share varies 46-63 ms/s
between launches of the same code. The bench (-4%) is the measure of this step; the profile only says no worse.

A/B: 5 v 5 launches of 150 s, alternating, after first (`ab_reverbsel_runs.txt`, `ab_reverbsel_lows.txt`):

| | before (30084e1) | after |
|---|---:|---:|
| FPS (steady periods, mean / median) | 62.6 / 62.6 | 62.8 / 62.7 |
| 10% low | 61.8 | 62.0 |
| 1% low (mean / median) | 59.3 / 61.0 | 59.4 / 61.1 |
| private memory (mean) | 2583 MB | 2572 MB |

No difference: rank z +0.42 to +1.69, none against. The city stays GPU-bound on the Radeon.

## Next

To look at next: what the loop's compiled instructions are spent on (its filters running side by side in vector
lanes is not established: the compiler interleaved them, nothing found repeated yet).
