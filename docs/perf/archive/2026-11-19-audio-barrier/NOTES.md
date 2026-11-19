# 2026-11-19: XAudio's mix worker rendezvous waits blocked instead of spinning (Crackdown)

The first function profile (docs/perf/archive/2026-11-19-profile, titles/crackdown/HOT_FUNCTIONS.md) put the
largest item of Crackdown's processor time on XAudio's mix worker sub_82A509D8: about 125 ms a second of a core, all
in Crackdown's `AudioBarrier` (its hook of sub_82A50000, the two mix workers' rendezvous once an audio pass). The
first to arrive spun (`_mm_pause`, a yield every 4096) until its partner did; the partner mixes more and comes
hundreds of microseconds later. The owner picked it as the first item of the table.

## The change (Crackdown src/xaudio_barrier.h, src/audio_guest_sync.cpp)

`XAudioBarrier::Wait`: a short spin (256 pauses, a few microseconds, no clock read), then asleep on a condition
variable until the releasing worker wakes it (the release takes the barrier's lock before it notifies: no sleeper
misses it), looking at the title's termination every 10 ms (a partner that stopped never releases). A first version
spun 20 us reading the clock each turn: that spin and its clock reads were most of what was left (about 25 ms/s),
hence the counted pauses.

## Tests

`xaudio_barrier_tests` (Crackdown tests/, `tools/test.ps1 -Filter xaudio_barrier`): the existing rendezvous checks,
and new ones: 20,000 rounds of two workers through Wait with one now and then a millisecond late (none lost); a worker
whose partner is 100 ms late uses 0.00 ms of processor time and wakes 0.007-0.014 ms after the release; a worker whose
partner never comes leaves within two looks once the title terminates. (Two other Crackdown tests fail before and
after the change, unrelated: guest_layout_tests, 36 older bindings missing from config/tu6/address-map.csv, and
packaging_tests.)

## In game (Radeon, cores 8-15, city route, records on)

Profile (2 launches each, 80 s, `profile_before_*`, `profile_after_*`; Any360's function profiler):

| | before | after (final) |
|---|---:|---:|
| AudioBarrier under sub_82A509D8 | 10.0 / 8.4 s of 80 s (about 125 ms/s) | 0.82 / 0.81 s (about 10 ms/s) |
| processor time caught running | 1.19 / 1.08 cores | 1.07 / 1.05 cores |

(The running total varies more between launches than the change: the barrier's own time is the measure.)

A/B, 10 v 10 launches of 150 s, alternating, after first (`ab_barrier_final_runs.txt` = `ab_barrier2_runs.txt` +
`ab_barrier3_runs.txt`, and their lows):

| | before | after |
|---|---:|---:|
| FPS (steady periods, mean / median) | 63.1 / 63.1 | 63.1 / 63.1 |
| 10% low | 62.3 | 62.2 |
| 1% low (mean / median) | 59.9 / 61.6 | 59.8 / 61.6 |
| private memory | 2583 MB | 2585 MB |

No difference: rank z between -0.72 and +0.86. The city is GPU-bound on the Radeon; the gain is processor time
given back (a tenth of a core), which counts on a 4-core Steam Deck. The two 5 v 5 halves leaned opposite ways
(steady FPS z -1.76, then +1.86): the records mode's random 35 FPS stretches, in 9 of 10 and 8 of 10 launches.
The first version's A/B (20 us spin, `ab_barrier_runs.txt`) was unchanged too.
