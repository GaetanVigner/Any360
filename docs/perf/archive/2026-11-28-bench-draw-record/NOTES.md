# Allocations in the bench; the renderer's draw hand-off and texture sets (2026-11-28)

Two changes. The bench now reports host allocations, as every measure must. Then come the next costs on the
renderer's thread after `../2026-11-28-bench-draw-cost/`: "record and descriptors" at 0.50 ms a frame and "submit"
at 0.46.

## 1. Host allocations in the bench

- `any360::sys::ProcessAllocations()` (`any360/sys/alloc_meter.h`): every thread's `operator new` calls and bytes,
  threads that ended included. Each thread counts into its own block, written by that thread only. The blocks are
  linked through themselves, so registering a thread allocates nothing, and they are summed on request: the
  allocation path takes no shared counter. The C runtime's `malloc` called directly and the Vulkan driver's own
  allocations are not counted.
- `any360_bench` reports host allocations and bytes a frame over the measured frames, in each period's first line
  and in `--json` (`per_frame.host_allocations`, `host_allocated_bytes`); see docs/BENCH.md. `--check` is unchanged.
- Test: `sys_object_tests` ProcessAllocationTests.
- City, NVIDIA, before part 2: 169 allocations (9.5-19.5 KB) a frame.

## 2. Where "record" and "submit" went

With recording threads (4 on this machine), a draw on the renderer's thread does not record Vulkan commands. It
prepares a job and hands it to a recording thread in chunks. "submit" (`ns_submit`) times that hand-off, and "record"
(`ns_draw_record`) includes it. The hand-off was split by temporary timers (not kept), ms a frame:

| piece | ms a frame |
|---|---|
| `Record()` and the render pass key | 0.15 |
| `GrowDrawn` (marking EDRAM rows written) | 0.13 |
| rendering begun when the targets change | 0.03 |
| the job copied into the chunk | 0.04 |
| `DispatchChunk` (a chunk of 32 draws queued, a sleeping recording thread woken) | 0.15 |

Allocations a frame by thread (temporary counters): the recording threads 127, the renderer's thread 17. The
recording threads' allocations came from their texture-set caches: an `std::unordered_map` keyed by `std::vector`,
emptied at each batch. Every new set allocated its key's vector and a node.

## The change

- `any360::gfx::FlatKeyMap` (`flat_key_map.h`, new) maps a short sequence of words to a value. It uses open
  addressing over one array of slots, with the keys' words in one array beside it. `Clear` keeps both arrays'
  capacity, so a batch with as many sets as before allocates nothing. It replaces both texture-set caches: each
  recording thread's (`RecordWorker::sets`) and the renderer thread's (`batch_sets_`, used without recording
  threads).
- `kChunkDraws` 32 -> 128: a chunk's queueing and its thread's wake-up (about 1.9 us) happen 20 times a frame
  instead of 79. A render pass's draws are still spread over the recording threads (the city's main pass: 16
  chunks). A pass of fewer than 128 draws now goes to one thread, recorded while the pass ends.
- Bench title: before unregistering its audio client at the end, the title waits until the client has submitted
  the 64 audio frames the check hashes. A fast run (240 frames in about 0.3 s) ended before the audio clock got
  there, and `--check` then failed on the audio hash, whatever the renderer did.

Tried and dropped: recording threads counted from the processors the process may run on (`--any360_cpu_affinity`)
instead of the hardware threads. On 2 processors that gives no recording thread (everything recorded on the
renderer's thread). One launch gave 120 FPS against 176-194 FPS for three launches with the usual 4 threads, so
the count stays on the hardware threads.

## Tests

- `gfx_flat_key_map_tests` (new, registered with ctest and `layer\sys\test.ps1`): every key found with its value and
  every absent key not found, against `std::map`, over keys of 1 to 33 words with shared prefixes, through growth and
  clears; 400 keys refilled after a clear allocate nothing. Its batch model (2,500 lookups over 80 sets, `--bench`,
  2,000 batches): FlatKeyMap 41-42 us and 0.01-0.30 allocations a batch, the `unordered_map` of vectors 73-76 us and 160 allocations (1.8x; 1.26x in the suite's run on the loaded machine).
- `any360_bench --frames 240 --check bench/expected/city.txt`: passes before and after (with the title's audio
  wait), picture unchanged.
- The suite, NVIDIA and Radeon under the validation layer: green (`layer\sys	est.ps1 -Gpus NVIDIA,Radeon`), 0 validation messages on each GPU.

## Measured

Two executables, alternated 6 launches each (`runs-whole.txt`, `runs-small.txt`, an exact two-sided Mann-Whitney rank
test in `ab-summary.md`). "before" is e9efd4b: origin/main plus part 1, so both variants report allocations.
"after" is part 2. `any360_bench --frames 1200`, headless, uncapped, 2,484 draws a frame. The machine was shared
with other sessions (about 80% CPU load): single launches varied by up to 2x.

Whole machine (16 threads, RTX 5070 Ti), medians of 6:

| | before | after | change | p |
|---|---|---|---|---|
| host allocations a frame | 168.9 | 41.8 | -75% | 0.002 |
| host bytes allocated a frame | 9,499 | 2,869 | -70% | 0.002 |
| draws, ms a frame | 1.42 | 1.28 | -10% | 0.015 |
| us a draw (draws / 2,484) | 0.57 | 0.51 | -10% | 0.015 |
| renderer's thread, ms a frame | 1.67 | 1.50 | -10% | 0.015 |
| FPS | 599 | 668 | +12% | 0.017 |
| median frame | 1.63 ms | 1.46 ms | -11% | 0.030 |
| 10% low | 468 FPS | 514 FPS | +10% | 0.065 |
| 1% low | 392 FPS | 430 FPS | +10% | 0.18 |
| CPU (100 = a core) | 338% | 326% | -4% | 0.026 |
| FPS per CPU% | 1.80 | 2.07 | +15% | 0.015 |
| GPU (system / batches) | 13% / 11% | 10% / 8% | | 0.002 |
| GPU ms a frame | 0.186 | 0.120 | -36% | 0.002 |
| private memory | 984 MB | 958 MB | -2.6% | 0.002 |

The lows move with the average but not significantly (p 0.065 and 0.18). The GPU's time a frame drops by a third:
fewer secondary command buffers a frame (20 chunks instead of 79) to execute.

Small configuration (`--any360_cpu_affinity=0x3 --any360_gpu_device=Radeon`). The first series (`runs-small-contended.txt`)
lost its last 5 launches of each variant to other work on the Radeon (GPU 15-25 ms a frame, against 1.5-2.2 ms).
The second series (`runs-small.txt`) lost two "before" launches the same way (GPU 6.2 and 34.5 ms a frame).
`runs-small-clean.txt` is the second series without them (4 v 6):

| | before | after | change | p |
|---|---|---|---|---|
| host allocations a frame | 143 | 26.4 | -82% | 0.010 |
| draws, ms a frame | 5.07 | 3.57 | -30% | 0.038 |
| us a draw | 2.04 | 1.44 | -30% | 0.038 |
| renderer's thread, ms a frame | 5.89 | 4.18 | -29% | 0.038 |
| FPS | 127 | 151 | +19% | 0.26 |
| median frame | 6.23 ms | 5.32 ms | -15% | 0.11 |
| 10% low | 92.9 FPS | 97.4 FPS | +5% | 0.76 |
| 1% low | 19.6 FPS | 26.1 FPS | +33% | 1.00 |
| CPU (100 = a core) | 83% | 86% | +4% | 0.91 |
| FPS per CPU% | 1.45 | 1.77 | +22% | 0.019 |
| GPU ms a frame | 1.51 | 1.44 | -4% | 0.067 |
| private memory | 1,081 MB | 915 MB | -15% | 0.005 |

On two processors the renderer's thread and the allocations improve significantly. Frame rate and lows do not
reach significance on this shared machine. Private memory drops by 166 MB there (26 MB on the whole machine), most
likely the driver's memory for the secondary command buffers. With chunks 4 times larger, each recording thread
allocates about a quarter as many for each of the 8 batches.

Allocations left (whole machine, 42 a frame): 13 on the renderer's thread (a draw's job and frame bookkeeping),
the rest on the title's and the system's threads.
