# 2026-11-19: the XMA decoder's output conversion in SIMD; a frame decoder, a capture and a bench to measure it

After the reverb steps, `sub_82A52850` (XAudio's XMA register writes) led the table's XDK audio rows: its time is
Any360's `AudioKernel::XmaWriteRegister`. A write to the kick registers (0x650-0x657) decodes the context's input on
the title's thread, as the hardware's kick does (any360/sys/xma.h's XmaDecoder: FFmpeg's xmaframes decoder, the FFmpeg
the ReXGlue SDK ships).

## What was there to measure, and what it showed

- `XmaFrameDecoder` (any360/sys/xma.h): the codec part of a context (FFmpeg opened for a stream's rate and channels,
  one frame decoded, its samples converted to big-endian 16-bit interleaved) taken out of `XmaDecoder::Context`.
  The context keeps one; `xma_bench` replays frames through one.
- `ANY360_XMA_CAPTURE=<file>`: every frame given to the decoders, and every stream reset, appended to a local file.
  A capture holds a title's audio and is never committed.
  - Crackdown's city route, 150 s: 110108 frames (51970 stereo), 718 resets, 15 contexts.
- `xma_bench <capture>`: the frames replayed. It reports:
  - nanoseconds a frame;
  - the host heap's allocations a frame, the C runtime's malloc family counted through the program's import table
    (FFmpeg allocates with av_malloc, which operator new counters miss);
  - a hash of every sample decoded, so that a change must give the same output bit for bit.
- `ANY360_XMA_STATS=<file>` (with `ANY360_XMA_VERIFY=1`, `ANY360_XMA_SCALAR=1`): in game, kicks, empty kicks,
  frames, nanoseconds in the kicks and in the frame decoder, each frame's conversion checked against the scalar
  statement, or the scalar conversion forced.

Where the frame decoder's time goes (`profile_decoder_bench.txt`, 82086 samples of the bench, the RelWithDebInfo
libavcodec):

| | share |
|---|---:|
| FFmpeg's WMA Pro decoding: `decode_frame` 29%, `ff_wma_run_level_decode` 11% | 40% |
| FFmpeg's transforms and windows, its C versions (the SDK's FFmpeg is built without its x86 assembly): `ff_imdct_half_c` 9.4%, `pass` / `fft8` / `fft16` 12.6%, `vector_fmul_window_c` 4.8%, `vector_fmul_scalar_c` 3.1%, the rest | about 31% |
| the output conversion (`XmaFrameDecoder::Decode`, scalar) | 10% |
| the heap (FFmpeg's send / receive packets and frames: 14 C runtime allocations a frame, 2 KB) | about 8-10% |
| reopening the codec on each stream reset (MDCT, FFT and VLC tables) | about 4.5% |

## The change

`XmaFrameDecoder::Convert` clamps, scales, truncates and byte-swaps four samples at a time (SSE, SSSE3's byte shuffle
packing each result's low 16 bits as the scalar truncation does, a NaN giving 0). `ConvertReference` is the scalar
statement it replaces.

## Tests

- `xma_tests`: 0 failing.
  - The conversion against its scalar statement, every byte: 400 frames of random samples and edges (the clamp's
    bounds and just past them, infinities, NaNs, denormals, signed zeros), mono and stereo.
  - The suite (`layer\sys\test.ps1`) does not build xma.cpp in a tree whose reference SDK has no FFmpeg headers, so
    these were run from build folders configured with them (`out/sys-xma`, `out/sys-xma-rel`).
- `xma_bench`: the same output hash before and after, 47221808C06B2A03 over the capture's 110108 frames.
- In game (`xma_stats_verify.txt`): 59108 frames' conversions checked against the scalar
  statement, 0 bytes different.

## Bench (Release, the shipped libavcodec, cores 8-15; `bench_alternated_release.txt`)

| | ns a frame | C runtime allocations a frame | operator new |
|---|---:|---:|---:|
| scalar conversion (before) | 7507-7556 | 14.06 (2015 B) | 0 |
| SIMD conversion | 6963-7000 | 14.06 (2015 B) | 0 |

That is -7%. The allocations are FFmpeg's own.

## In game (Radeon, cores 8-15, the city route; `xma_stats_*.txt`, one launch each)

Nanoseconds a frame, at each launch's snapshot (20000 or 40000 kicks):

| launch | conversion | in the frame decoder | in the kick |
|---|---|---:|---:|
| scalar1 | scalar | 10050 | 11532 |
| vector1 | SIMD | 9465 | 10761 |
| scalar2 | scalar | 9499 | 10784 |
| vector2 | SIMD | 8593 | 9792 |
| verify (+ checking, + profiler) | SIMD | 12106 | 13672 |

- The game's frames cost 8.6-12 us against the bench's 7: 15 contexts' decoders (hundreds of KB each) interleaved
  with the game run out of cache.
- The kick's own work around the decoder (packet parsing, buffer clears, the output ring) is about 1.3 us a frame.
- Launch-to-launch spread (8.6-10.1 us) is larger than the conversion's gain (about 0.5 us), so the bench is this
  step's measure. A frame-rate A/B cannot see it either.

## Left, with their ceilings

- Reusing the codec across a stream reset instead of reopening it: xmaframes has no flush. A fresh state would mean
  copying a snapshot of its private context (hundreds of KB, its layout an FFmpeg internal) for about 2%. Not done.
- Calling the decoder's callback instead of send / receive: FFmpeg's ff_get_buffer still allocates three times a
  frame, and it leans on FFmpeg internals, for at most 7%. Not done.
- The transforms in SIMD, or an FFmpeg built with its x86 assembly: up to about 31% of the frame decoder. It changes
  the output's bits (a different order of floating-point operations), so it is the owner's decision.
