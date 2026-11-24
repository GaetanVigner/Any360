# 2026-11-24: who spins on Sleep(0), and an idle back-off (off by default)

After the zero-delay fast path and spin pacing ([2026-11-23-spin-pacing](../2026-11-23-spin-pacing/NOTES.md)),
Mirror's Edge still made about 2 million zero delays a second. A per-thread diagnostic (in the log: a thread that spent
more than a tenth of a second in zero delays, with its start address, its calls and its longest run) named them
([spinning_threads_diag.txt](spinning_threads_diag.txt), one minute of play):

| Thread (start) | Share of a processor in zero delays | Calls a second | Longest run |
|---|---|---|---|
| 4103 (82385C28) | median 74%, up to 95% | 1.37 million | 170 ms |
| 4102 (82385C28) | 16-21% (5 s of 153) | 0.73 million | 24 ms |
| 8 (the main thread: the frame limiter, paced) | 16-26% | 0.14 million | 129 ms |

82385C28 starts Unreal Engine 3's runnable thread; its Run (`sub_82371F78`) is the render thread: it reads its
command queue's pointers in memory, and when the queue is empty calls Sleep(0), reads the clock, adds the time to its
idle counter and loops. The game thread writes the queue straight into memory: no kernel call to wake on.

## The host's timer

`sys_abi_bench` (PreciseSleep, a high-resolution waitable timer, 200 sleeps each): 50, 100 and 200 us asked take
~513 us (99th percentile 570-1213 us), 500 us ~1004 us, 1 ms ~1517 us: Windows sleeps in 0.5 ms steps. A sleep shorter
than half a millisecond does not exist here; any back-off slice is 0.5-1 ms.

## The idle back-off

`SpinPacer` (`any360/sys/spin_pacer.h`), `--any360_sys_idle_backoff` (with spin pacing): a thread whose waits have no
cadence (16 streak ends in a row not even roughly periodic) and whose current wait has gone on for 2 ms sleeps in
slices (1/8 of what it has waited, 100-500 us asked). Streaks end at a 50 us gap between zero delays (an empty poll
takes a few microseconds). Tests on a simulated clock with Windows' 0.5 ms steps: a thread polling for work at random
times sleeps 84% of its idle time (zero delays 21.4 -> 4.7 million), sees work at worst 1.36 ms late after waits
over 2 ms and at once after shorter ones; the frame limiter keeps its exact cadence (0 of 260 frames late); a render
thread whose waits end at a cadence within ~1 ms is left to yield (0 of 560 frames late).

In game, Mirror's Edge 1440p MSAA 4x, spin pacing on in every run:

| A/B | Back-off off | Back-off on | Rank test |
|---|---|---|---|
| Played (same route, 4-5 runs each, [ab_manual_play_backoff.txt](ab_manual_play_backoff.txt)): CPU (profiles, 3 off / 2 on) | 1.46-1.50 cores | 1.15-1.31 | too few |
| Played: 1% low / 10% low | 52.1 / 58.5 FPS | 52.4 / 58.15 | p = 0.54 / 0.17 |
| Standing in the level (scripted, 5 runs each, [ab_auto_standing_backoff.txt](ab_auto_standing_backoff.txt)): CPU | 1.61 cores | 1.57 | p = 0.24 |
| Standing: 1% low / 10% low | 54.4 / 58.9 FPS | 52.0 / 58.15 | p = 0.087 / 0.18 |

In play the render thread's waits are irregular and the back-off sleeps 3.4-5.7 s of every 10 s on it; standing, its
waits come once a frame (~11.6 ms) and the back-off leaves them. The gain is not shown well enough (too few played
profiles, a 1% low that may be lower standing): the back-off stays off by default until a played A/B with enough
runs shows it (an input recorder would make played runs automatic).

## Rejected: predicting roughly periodic waits

The render thread's waits standing (~11.6 ms, once a frame, ending within ~1 ms of their cadence) spin ~70% of a
processor that neither the exact prediction (kJitter 250 us) nor the back-off takes. Predicting them with a margin
widened by the periods' spread was tried: scripted, 5 runs each against everything off
([ab_auto_standing_wide_prediction_rejected.txt](ab_auto_standing_wide_prediction_rejected.txt)): 1.48 -> 1.30
cores (p = 0.008) but the 1% low 55.9 -> 33.8 FPS, FPS 62.0 -> 61.5 (58-84 waits a run ended at a sleep's return:
the frame's commands seen a frame late). Removed; the test keeps that pattern yielding.

## Automated runs

`--any360_input_script=18:start/200,22:a/200,26:a/200,30:a/200,34:a/200,38:a/200` takes Mirror's Edge minimized into
its first level in ~40 s, standing (2930 draws a frame); 80 s a run, the profile at 50-70 s. Standing is
reproducible but not play: its render thread waits differently.

## Played-route A/B: on by default (2026-11-24, later)

With recorded runs (docs/perf/archive/2026-11-24-input-recording/): the owner's route 2 (menus by the level
selection, then ~70 s of safe play) played back minimized, 12 alternating launches, spin pacing on in all, the profile
on 60 s of play (40-100 s). Every run's draws a frame followed the recording's to within ~2% but one (run 2 with the
back-off, off the route from ~50 s: left out; with it the result is the same). [ab_route2_backoff_valid.txt](ab_route2_backoff_valid.txt)
([all runs](ab_route2_backoff_all.txt), profiles and logs in [route2/](route2/)):

| | Back-off off (6) | Back-off on (5) | Rank test |
|---|---|---|---|
| Process CPU | 1.52-1.67 cores, median 1.575 | 1.33-1.46, median 1.37 | p = 0.004 |
| FPS / median frame | 61.95 / 16.12 ms | 61.95 / 16.12 ms | p = 0.69 / 0.96 |
| 10% low / 1% low | 58.65 / 52.5 FPS | 58.75 / 53.1 FPS | p = 0.63 / 0.83 |
| Time under KeDelayExecutionThread (60 s) | 7.6-8.4 s | 3.8-4.9 s | |

0.2 of a processor less, the lows the same: `--any360_sys_idle_backoff` is on by default.
