# XAudio's I3DL2 reverb native (2026-11-18)

`any360::xdk::XAudioReverb` (any360/xdk/xaudio_reverb.h), bound in Crackdown at 0x82A4DCF0: one frame of the
effect (256 samples, two channels in, four out) through its ten stages.

## Version

The function's own statements, made into one host leaf by `tools/ppc_model.py --native` from Crackdown's recompiled
code: every stage forced inline, the registers in a context local to the call, memory reached without the runtime's
volatile accesses, the leaf compiled twice (with FMA3 for the build's `std::fma` calls, and without). The model
(`xaudio_reverb_reference.h`) is the same tool's output without `--native`: the recompiled statements on
`ppc_model.h`'s context.

## Off-game (xdk_xaudio_reverb_bench.txt)

| | model | native | |
|---|---|---|---|
| µs a frame (median of 5 runs, 2,000 frames each) | 85.95 | 78.32 | 1.10x |
| bytes / allocations a call | 0 / 0 | 0 / 0 | |

## In game (in_game_verify_bench.txt)

Crackdown's city, scripted route, NVIDIA RTX 5070 Ti, cores 0-7.

| | title's code | native | |
|---|---|---|---|
| verified calls | 51,402 | | 0 different, none settled on a second try |
| µs a frame (51,690 timed calls) | 108.7 | 94.4 | 1.15x |
| bytes / allocations a call | | 0 / 0 | |

About 430 frames a second (several reverb voices): the native saves about 6 ms each second of the audio thread's time.
That is below what a frame-rate A/B resolves (the city runs at its 60 FPS cap on this machine), so none was run.

## Found on the way

- The state is 0x4B9D0 bytes, not the 0x4B5D0 first measured: the effect keeps a 256-sample buffer (1 KB) after its
  delay lines, written by one frame and read by the next. The first in-game check gave 0 different but 83% of the
  calls "different at first and the same when run again": that buffer was neither compared nor put back between the
  two sides, so the side that ran second read what the first had just written. Found with a probe that runs the
  title's code twice on the same input (it gave itself other bytes, the native's), then by the off-game test, which
  now fails any byte written outside the call's plan.
- The verifier (`RunSides`) now starts each side with the host's floating-point control word the call found: both
  the recompiled code and the leaf write MXCSR only when the guest's flush mode changes, so a side started with the
  other's leftover mode could compute denormals differently than its context says. Not the cause above, but a real
  hole for every function run that way.
- Crackdown's window stayed black at the title and in the city on 2026-11-18 (the game ran, drew and took input);
  the renderer is unchanged since the last run the owner saw, and the black stayed with every native off. Open, not
  looked into further here.
