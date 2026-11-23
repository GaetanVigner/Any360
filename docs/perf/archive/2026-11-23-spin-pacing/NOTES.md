# 2026-11-23: zero delays (Sleep(0)): a fast path and spin pacing

After the GPU resolves and the sample pool, Mirror's Edge at 1440p (host MSAA 4x) runs at its 62 FPS cap and its
biggest CPU cost left was in `KeDelayExecutionThread` with a zero interval: 5.8 s of 30 s of host code under it, with
`RtlQueryPerformanceCounter` 4.1 s and locks (`Mtx_lock`, SRW) 2.3 s. Every Sleep(0) went through `Scheduler::Delay`:
the kernel APCs' delivery (a lock), the stack monitor (a clock read), the thread's lookup for its suspension (a lock and
a search), then a yield. The game's frame limiter loops on `QueryPerformanceCounter` + Sleep(0) until its frame time,
and other threads (its job system) yield about 2 million times a second with no cadence.

## 1. The fast path (every title, always on)

- The suspension check reads the thread's suspend count first: no lock and no search when it is 0.
- The APC queue keeps its size in an atomic: a wait or delay with no APC queued for any thread takes no lock.
- The stack monitor looks at every delay that sleeps and at every 64th zero delay of a thread (a per-thread count).

`sys_abi_bench 9 KeDelay` (fastest of 9 runs, 3 runs each; [before](sys_abi_bench_before.txt),
[after](sys_abi_bench_after.txt)); the original runtime's KeDelayExecutionThread needs a thread of its own runtime (it
faults on the bench's), so the host's `std::this_thread::yield` alone is the comparison:

| Call | Before | After | Host yield alone |
|---|---|---|---|
| KeDelayExecutionThread (0) | 214.0-216.2 ns, 0 B, 0 allocations | 189.8-198.2 ns, 0 B, 0 allocations | 173.5-198.1 ns |
| KeDelayExecutionThread (0, alertable) | 215.7-223.4 ns | 190.1-196.2 ns | |
| KeDelayExecutionThread (0), spin pacing on, no cadence | | 221.9-227.7 ns (two clock reads), 0 B | |

## 2. Spin pacing (`any360/sys/spin_pacer.h`, `--any360_sys_spin_pacing`, on by default)

A thread's non-alertable zero delays that wait at a steady cadence (a frame limiter) sleep, on a high-resolution timer,
until a margin before the predicted end of the wait, then yield as before; threads without a cadence only yield.
Alertable zero delays always yield (an APC queued meanwhile is not held back). Tests (`sys_object_tests`, a simulated
clock): at 62 FPS 0 of 280 frames late and 88% of the wait slept; with 3 ms overshoots 1 frame late by 2 ms of 380
(3 spikes); a job system's random yields never sleep (51,600 zero delays); 30 to 62 FPS: one frame late by up to the
old period at the switch, then 0 late and 82% slept; heavy stretches 0 late.

In game, Mirror's Edge, first level, same route, 12 alternating launches (pacing on: runs 1, 3, 5, 7, 9, 11; off: 2,
4, ..., 12), the profile on 20 s of play in runs 4-12 ([ab_mirrorsedge.txt](ab_mirrorsedge.txt), the profiles and
logs in [profiles/](profiles/)):

| | Pacing off | Pacing on | Rank test |
|---|---|---|---|
| Process CPU (20 s profiles, 5 off / 4 on) | 1.58-1.62 cores, median 1.60 | 1.45-1.55, median 1.525 | p = 0.016 |
| FPS (median of the runs' periods) | 62.0 | 62.0 | p = 1.0 |
| Median frame | 16.13 ms | 16.12 ms | p = 0.36 |
| 10% low | 58.25 FPS | 58.5 FPS | p = 0.09 |
| 1% low | 52.8 FPS | 52.8 FPS | p = 0.94 |
| Slept a 10 s report (the limiter's thread) | 0 | 1.9-2.4 s | |
| Waits that ended at a sleep's return (margin grew) | | 6-13 a run | |

The limiter's thread now sleeps about 2 s of every 10 s instead of spinning; the process uses 0.075 of a core less
and the frame times and lows are the same. What is left is the job system's threads: about 2 million zero delays a
second with no cadence, which the pacer leaves to yield, and on which pacing adds its two clock reads (the time under
KeDelayExecutionThread in the profiles is 2.5-2.8 s with pacing against 1.7 s without). Next: cheaper time stamps
for the pacer, and those idle threads (a generic back-off for long cadence-less streaks, measured on job pickup).
