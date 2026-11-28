# Waits and yields on few processors (2026-11-28)

The bench's small configuration (2 processors, the Radeon iGPU) looked like Any360's threads slept while frames were
late: 100-157 FPS, 1% lows 23-27 FPS, the process at 33-75% of its 2 processors
(`../2026-11-27-bench-city-baseline/`). This step profiled who waits and when, and changed what the measurements
supported: the spin pacer's idle back-off starts after 0.2 ms of yielding instead of 2 ms, and `any360::sys` turns
spin pacing and the back-off on by default, as titles already run.

## What the profile showed

Tools (in `tools/`): `thread_states.py` samples a process's thread states and wait reasons
(`NtQuerySystemInformation`, about 60 samples a second, no administrator rights needed), `stacks.py` prints every
thread's stack of a live process (dbghelp `StackWalk64`), `ab.ps1` runs alternating A/B launches.

1. **The symptom was other processes, not Any360 sleeping.** Processors 0-1 were busy all day with other sessions'
   builds and tests (some pinned to 0x3; the machine at 70-97%). The same binary gave 82, 102, 152 and 174 FPS on
   0x3, and once 6 FPS with the bench's threads ready 77-95% of the time and almost never running. On two idle
   processors of the same shape (0xC000: processors 14-15, one core's two SMT threads, given to this step by the
   owner) the bench used 97-99% of them with 0 frames over 16.7 ms. The baseline's NOTES paragraph is corrected.
2. **A yield does not give the processor away.** The title's render thread waits on its fence and on ring space with
   zero-interval `KeDelayExecutionThread` calls (`std::this_thread::yield`, `SwitchToThread`): 1.5-4.5 million a
   2 s on 2 processors, of which 0.03-2% switched to another thread, and more than 99.7% returned within 5 us.
   `SwitchToThread` only hands over to a thread waiting on the same processor. Meanwhile the renderer's thread (the
   frame's bottleneck, about 4.5 ms of draws a frame) sat ready without a processor 29% of the time.
3. **The bench ran without the titles' spin pacing.** `Scheduler` defaulted spin pacing and the idle back-off to off;
   only the ReXGlue host turns them on (its `any360_sys_spin_pacing` and `any360_sys_idle_backoff` settings, on by
   default). So the bench measured plain yields while titles run the pacer.

Rejected:

- `Sleep(0)` instead of `SwitchToThread` (alone, or after a yield that did not switch): it only gives way to threads
  of equal or higher priority, starved the lower-priority renderer threads (106 FPS, 324 frames over 16.7 ms, 45% of
  the processors) and one launch never ended. Not kept.
- Raising the host timer resolution (`timeBeginPeriod` with the timer's minimum period when the scheduler makes its
  first thread): no gain on the bench, 5 v 5 on 0xC000 (FPS z -0.9, 1% low z -0.5, `ab2-0xC000/summary_rank.txt`,
  variants `base` and `timer`). Not pushed. See "A requirement for the native host" below.

## The change

- `SpinPacer::kIdleAfter`: 2 ms -> 0.2 ms. A thread whose zero delays have no cadence (kAperiodicEnds streak ends in
  a row not even roughly periodic) sleeps slices once a wait has gone on for 0.2 ms; the sleep gives its processor to
  the process's other threads. Waits shorter than 0.2 ms still only yield. The aperiodic gate stays: frame limiters
  and render threads waiting at a cadence are never sliced (the Mirror's Edge 1% low depends on it,
  `../2026-11-24-idle-backoff/`).
- `Scheduler`: spin pacing and the idle back-off on by default (any host, as the ReXGlue host already sets them).
- The ReXGlue setting's description says 0.2 ms.
- Tests: SpinPacer's simulated-clock back-off test now has waits under 0.2 ms (still seen at once, by yielding) and
  checks that every wait over 0.4 ms is sliced; the frame limiter, cadence and render-thread tests are unchanged and
  pass (`sys_object_tests`).

## Measurements

Variants were switched by environment variables in an experiment build (`tools/experiment-switches.patch`, not
pushed): `base` = the bench as it was (pacing off), `pacing` = the titles' pacing (back-off after 2 ms), `after200`
= pacing with the back-off after 0.2 ms (this change), `short` = after 0.2 ms with the aperiodic gate off. Launches
alternate variants, `--frames 2400` (2,280 measured), each held until processors 14-15 were under 15% busy, at
AboveNormal priority from A/B #5 on (unpinned builds also land on 14-15). Figures are medians of the end-of-run lines;
z is a Mann-Whitney rank test on the launches (`tools/summary_rank.py`); `frame_lows.txt` is the periods' analysis.

### Small: 0xC000 + Radeon (A/B #5, 6 v 6, `ab5-0xC000/`)

| variant | FPS | median ms | 10% low | 1% low | frames over 16.7 ms |
|---|---|---|---|---|---|
| base (pacing off) | 288.5 | 3.6 | 232.7 | 186.9 | 0 |
| after200 (this change) | 327.8 | 2.9 | 266.3 | 212.8 | 0 (one launch 1) |
| short (gate off) | 345.8 | 2.8 | 310.2 | 225.8 | 0 (one launch 1) |

- after200 against base: FPS z +2.9, median z -2.9, 10% low z +2.6, 1% low z +1.6.
- short against after200: FPS z +2.1, 10% low z +2.9, 1% low z +1.8: the gate costs about 5% FPS and 16% of the 10%
  low on this bench. Dropping it is left to a later step with its own in-game check (it slices cadenced waits).

A/B #2 (5 v 5, before the rebase on 6841697, normal priority, `ab2-0xC000/`): base 196 FPS, 1% low 143 (mean of the
steady periods), 97% of the processors, 1.02 FPS per CPU%; pacing 214 FPS, 1% low 137; short 253 FPS, 1% low 189, 81%
of the processors, 1.57 FPS per CPU% (base against short: FPS z -2.9, 1% low z -2.5).

### What titles get: 0xC000 + Radeon, titles' pacing against this change (A/B #6, 5 v 5, `--frames 6000`, `ab6-0xC000-titles/`)

Titles already run pacing with the back-off after 2 ms; this is their delta. Steady periods (`frame_lows.txt`):

| variant | FPS | 10% low | 1% low | CPU% of the 2 processors | FPS per CPU% |
|---|---|---|---|---|---|
| pacing (2 ms) | 322.6 | 234.2 | 196.3 | 94 | 1.72 |
| after200 (0.2 ms) | 345.1 | 277.7 | 224.2 | 91 | 1.90 |

Rank tests: steady FPS, 10% and 1% lows z 5.0 (every period of one variant above every period of the other), dips'
1% low z 2.6, CPU% z 4.4; per launch, every after200 launch above every pacing launch in FPS and both lows.

### Whole machine: 0xFFFF + NVIDIA (A/B #16, 5 v 5, `--frames 6000`, `ab16-0xFFFF/`)

The titles' pacing against this change, the machine shared with other sessions (so wider spread):

| variant | FPS (steady) | 10% low | 1% low | CPU% of a core | FPS per CPU% |
|---|---|---|---|---|---|
| pacing (2 ms) | 524.0 | 464.6 | 416.8 | 314 | 1.67 |
| after200 (0.2 ms) | 559.0 | 480.2 | 420.4 | 261 | 2.14 |

No regression: steady FPS z +1.6, 10% low z +0.7, 1% low z +0.6 (all within noise, all in this change's favour), CPU
17% lower (z 2.7). The end-of-run 1% low leans the other way within noise (medians 423.5 against 413.4, z 1.4).

## A requirement for the native host

Without ReXGlue (whose SDL sets a 1 ms timer period for the process), a 1 ms `KeDelayExecutionThread` took 15.5 ms
and a 1 ms timed wait on an event took 15.5 ms (medians of 20, `sys_object_tests` on the experiment build); with
`timeBeginPeriod(1)` they took 2.0 and 2.0 ms, and the delay through the high-resolution waitable timer
(`PreciseSleep`) 1.5 ms. The bench does not show it (its title only yields), and titles get the period from
ReXGlue today. When titles leave ReXGlue, `any360::sys` (or its native host) has to set the host timer period itself,
or every short delay and timed wait a title makes sleeps a 15.6 ms tick.

## Also found

- The bench's end-of-run hang (the stop release lost on a full `kSemQueued`), already fixed on main by 6841697 when
  found; two launches here hung on the older base and were stopped.
- Black Ops' hitch traces (`F:\COD BO\routes\profile-2026-11-28\cpu\r1-v1-122631-diag`): the threads sampled in waits
  were running (about 63 ms of processor in a 108.8 ms frame), and the hitch sampler itself got 3-26% of its samples:
  processor contention or host stalls, not late wake-ups.
