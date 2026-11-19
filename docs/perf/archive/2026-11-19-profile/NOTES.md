# 2026-11-19: where Crackdown's processor time goes (the first function profile)

The owner asked for an analysis of which functions cost the most, individually and in total (a function called
often matters more than one called once), as a table kept in the repository and completed with the next functions.
The table: `titles/crackdown/HOT_FUNCTIONS.md` and `hot_functions.csv`, made by `tools/hot_functions.py` from these
runs.

## The tool: the function profiler (Any360 `any360/host/profiler.h`)

`--any360_profile_seconds=N` (with `--any360_profile_after=S`, default 60) samples every thread of the process every
2 ms for N seconds and writes `logs/profile.txt` and `profile.csv`. A thread caught running counts the time since the
previous round on its stack (the chance of catching a thread running is its share of the time, so the rounds' times
summed over its samples estimate the time it ran there); a thread caught in a system call (ntdll's Zw*/Nt*: a wait)
is counted apart. Per title function (a `sub_XXXXXXXX` symbol): its own time (innermost title function), the part in
its own code (self), and its time with its callees; per host function a title function calls (a binding's handler, a
native, a kernel export: a binding that only forwards leaves no frame, so a bound function's handler shows under its
caller), and per innermost host function. Unit-tested in `host_diagnostics_tests` (a thread spinning under a function
named `sub_8200ABCD` beside an idle one: first, its own time most of the stretch, the host function it calls under
it).

Two estimators were tried and dropped on the way (their runs not kept): putting a thread's cycles since the last look
on its current stack put most of the work on the waits the threads blocked in (63% on `ZwWaitForAlertByThreadId`,
the XAPI wait wrapper at the top); leaving the kernel samples out of that kept the rest's order but lost 70% of the
time. The interval weight was also first the nominal 2 ms, when a round takes about 3.5 ms (2 ms of sleep and the
captures): now the measured round time.

## The runs

Crackdown at Any360 cc9ab5a plus the profiler, its own A/B worktree's build (crackdown-amd-ab 75df800), the city route
(tools/any360-ab.ps1's input script), the Radeon iGPU, cores 8-15, D3D records on, seconds 60-140 of each launch:
- `profile_1.*`, `profile_2.*`: two launches; 1.19 and 1.08 cores caught running, 54 and 47 s of title functions in
  the 80 s (the orders agree; the table averages them).
- `call_counts_a.txt`, `call_counts_b.txt`: a third launch of a build that counts calls (`count_run.py`: the binding
  macros count theirs, `call_counter_core.h`; counting hooks on the profile's 563 hot unbound functions; never
  committed to the title): snapshots 62 s and 142 s after the start, 965 functions; calls a second = the difference
  over 80 s.

## What it says (the table has the numbers)

- The largest item is not work: Crackdown's AudioBarrier (the hook of XAudio's sub_82A50000) spins the two mix
  workers until both arrive: about 105 ms a second of a core under sub_82A509D8, a tenth of all the running time.
- Then audio natives and kernel calls: Any360's XAudioReverb (42 ms/s) and XAudioMixRamp (14 ms/s), any360::sys's
  XmaWriteRegister (20 ms/s, 60 us a call at 338 calls/s).
- Then game code: a cluster in 822xxxxx (sub_8225F9F8, sub_822608A8, sub_82260318, sub_82261F10: about 75 ms/s
  together, 2250 calls/s), sub_82252150 (0.55 ms once a frame), the command buffer submit sub_823B3328 (34 ms/s at
  17 K calls/s), sub_825BB3B8 and sub_825AA008 (once a frame).
- The D3D runtime's natives are each a few ms/s; the rest of the D3D runtime's unbound functions are far down.
