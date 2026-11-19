# 2026-11-19: XAudio reverb's hot loop with its memory in locals (tools/ppc_model.py --promote-memory)

The hot functions table (titles/crackdown/HOT_FUNCTIONS.md) put XAudio's reverb first after the AudioBarrier change:
`sub_82A4F410`, whose time is Any360's XAudioReverb native (bound at sub_82A4DCF0), 46-63 ms a second of a core
depending on the launch. The native is the function's own statements compiled as one leaf
(any360/xdk/xaudio_reverb_native.h, made by tools/ppc_model.py --native). The owner asked for the two hot loops to
keep their pointers and values in registers instead of going back to the guest stack and memory, bit for bit.

## The change

Generic, in the generator (tools/ppc_model.py), so that any title's loop made native this way can use it:

- `--promote <label>`: the loop's stack slots (r1 + K) in locals, loaded where it is entered and stored back where it
  leaves (checked statically: one way in and out, r1 unchanged, 32-bit slot accesses only, no stack address the
  function forms among the slots).
- `--promote-memory <label>`: the loop's memory at invariant addresses in locals. An abstract interpretation of one
  iteration finds every load and store either at an invariant address or in an invariant base plus a bounded range.
  It recognizes counters, induction slots and registers, masks and shifts, slot values forwarded within an iteration,
  and the ranges of words the loop rewrites (a ring's index). Each invariant carries a guess of its place,
  root + k (from the function's statements before the loop: here every base is the state + a constant). Words with
  the same place share one local. A word whose place has a spread (a double buffer's `+4 * (x & 1)`) stays in memory,
  as does any word it overlaps while one of them is written. The guess is checked at run time:
  - the first iteration runs the plain copy of the loop;
  - then one comparison per base (101), each root's words within the 32-bit space, and every access left in memory
    apart from the words in locals that matter;
  - if the check fails, the plain copy runs the rest.
  So the bits are the original's for every input. Counters `g_promoted_loops` / `g_plain_loops` and a test switch
  `g_force_plain_loops` (any360/xdk/ppc_model.h) say which path ran.

For the reverb's loop loc_82A4E774 (256 samples, 678 PowerPC instructions an iteration):
- 128 words in locals (46 written) on 2 roots, 201 of the 289 loads and stores gone;
- 70 accesses left in memory (the delay lines and the stack's work buffers);
- 1033 checks, about 1 us a call.

The leaf also copies its context and memory handle into locals (no aliasing through them).

## Tests

`xdk_xaudio_reverb_tests`:
- 681 checks, 0 failing: every byte of the state, the outputs, the stack below r1 and the caller's word, against the
  model, over 29 sequences in the three input modes and the dying-out (denormal) cases.
- The loop took the promoted path on all 312 regular frames.
- 24 more frames run with the check forced to fail: the plain copy, the same bits.

The loop's memory is the state and the stack's work buffers, so no valid input makes the check fail. Only a stack laid
over the state could, and the effect does not survive that: it writes over its own pointers, the model as well. Hence
the forced switch.

## Bench (`xdk_xaudio_reverb_bench 5`, cores 8-15)

Microseconds a frame (256 samples):

| | us a frame | vs the model |
|---|---:|---:|
| the model (the recompiled statements, any360/xdk/xaudio_reverb_reference.h) | 85-89 | 1.0x |
| native before (committed: one leaf) | 77.5 | 1.1x |
| stack slots in locals (--promote) | 65.3 | 1.34x |
| and memory in locals (--promote-memory), its loop plain (check forced to fail) | 66.3 | 1.32x |
| and memory in locals (--promote-memory) | 50.5 | 1.73x |

- Alternating `--promote` against `--promote-memory` 3 x 5 times: 65.3 against 50.4.
- The loop is promoted on 10000 of 10000 frames.
- 0 B and 0 allocations a call.
- Without a check and with every site promoted (wrong bits, measured only as a ceiling): 44 us. The 40 sites left in
  memory for the double buffers are what is left.

## In game (Radeon, cores 8-15, the city route, records on)

Verify launch (`--any360_xdk_verify`, `crackdown_203.log`):
- 33196 reverb calls compared with the title's own code: 0 different.
- The loop was promoted on all 33196, plain on 0.

Profile (80 s, `profile_before.*`, `profile_after.*`):

| | before | after |
|---|---:|---:|
| sub_82A4F410 (the reverb native) | 5069 ms of 80 s (63 ms/s) | 3374 ms (42 ms/s) |
| processor time caught running | 1.38 cores | 1.27 cores |

The reverb's share varies between launches: 46 ms/s in the AudioBarrier step's profiles, same code. The before/after
ratio here (-33%) matches the bench (-35%).

A/B: 10 v 10 launches of 150 s, alternating, after first then before first (`ab_reverbmem_final_runs.txt` =
`ab_reverbmem_runs.txt` + `ab_reverbmem2_runs.txt`, and their lows):

| | before | after |
|---|---:|---:|
| FPS (steady periods, mean / median) | 62.2 / 62.5 | 62.3 / 62.6 |
| 10% low | 61.2 | 61.3 |
| 1% low (mean / median) | 57.4 / 60.4 | 58.6 / 60.3 |
| private memory (mean) | 2588 MB | 2604 MB |

- No difference: rank z between -0.82 and +0.56. The city is GPU-bound on the Radeon; the gain is processor time
  given back on the audio thread (about 21 ms a second of a core).
- One after launch (`crackdown_215`) had a 636 ms frame with the command processor busy 290 ms: the records mode's
  random stretches (one before launch had 151 frames over 16.7 ms). The audio thread does not drive the command
  processor.
- The two 5 v 5 halves leaned opposite ways (1% low z +1.23, then 10% low -1.82).

## Next

- The double-buffer sites: pick between the two candidate locals with a selector computed at the check (ceiling
  about 44 us).
- Then the loop's vector form.
