# CPU writes to watched textures applied in kick order (2026-11-28)

The bench's city scene (docs/BENCH.md) showed 8.5 texture uploads a frame and 0.84-1.0 ms a frame in textures for one
256 KB tiled 8888 texture the title rewrites through the CPU view each frame (`City::Stream`, plus a DXT1 one every
60 frames), where one upload is expected (baseline: `../2026-11-27-bench-city-baseline/`).

## Why one rewritten texture gave about 8 uploads

- The watch reports one 4 KB page a fault (`GuestSpace::OnFault`), so a 256 KB rewrite is 64 reports.
- The title's main thread rewrites the texture of frame N (`City::Stream`) while the renderer's thread, a frame or
  two behind, still draws frames N-1 and N-2, which sample the same texture (all 16 textures are drawn each frame).
- The texture cache applied every queued report before the next texture use. Each draw that sampled the texture
  while it was being rewritten found new pages written: it armed the watch over the whole texture again, hashed the
  half-written 256 KB and uploaded it; the next page the title wrote faulted again. Uploads followed the number of
  times a draw found the rewrite in progress (about 8 a frame), and watched writes rose to 104 a frame for 64 pages.
- Every such application also bumped the cache's version, which voids the renderer's per-fetch-constant view memo for
  every texture, so all draws after it took the slow lookup.

## The bench host matches the game host

The ReXGlue runtime's physical memory callbacks (`PhysicalHeap::TriggerCallbacks`, reached from the access
violation handler with a length of 1 and `unwatch_exact_range = false`) report the 4 KB system page that faulted;
the game host's `MemoryWritten` (`layer/host/rexglue/src/rexglue.cpp`) returns exactly that range, so only that page
is unwatched; `EnablePhysicalMemoryAccessCallbacks` re-arms whole ranges. The reference SDK the titles link
(`tools/rexglue/win-amd64`, v0.10.0 with one patch, `0001-xam-tile-to-texture`) does not change that path. The bench's
`GuestSpace` does the same (one page a fault, the exact page unwatched, re-arming an armed page a no-op). The storm
is real in games that stream textures through the CPU, not an artefact of the bench host.

## The change

- `TextureCache::QueueCpuWrite` (new): the hosts report watched CPU writes there instead of `QueueMemoryChanged`.
  They are counted (`CpuWriteMark`) and applied only once the renderer is told a mark taken after them
  (`CpuWritesVisible`), so draws kicked before a write keep the content they were kicked with.
- `Renderer::CpuWriteMark` / `CpuWritesVisible` (new virtuals): the command processor marks each kick as it starts
  running it (ring kicks and direct submissions) and each WAIT_REG_MEM the CPU satisfies; the asynchronous renderer
  reads the mark on the command processor's thread and queues each new one, in order with the draws, to the
  renderer's thread (one 16-byte command a kick when the mark moved).
- Writes the GPU side reports (resolve write-backs, the command processor's own memory writes) still go through
  `QueueMemoryChanged` and apply before the next use. A host that never marks (replays, tests) keeps the old
  behaviour: every reported write applies at once.
- Arm-before-read is unchanged: a page that is unwatched always has a pending write queued or a stale texture.
- Hosts changed: `layer/host/rexglue/src/rexglue.cpp` (`MemoryWritten`) and `bench/src/gpu.cpp`
  (`BenchGpu::MemoryWritten`), one call each.

Rejected: coalescing at the swap or at the renderer's frame (`NewFrame`): a title that writes a texture and draws
with it in the same frame (glyph atlases, Lock/Unlock then draw, video planes) would show the old content for a
frame. The kick is the boundary the hardware has: a CPU write is visible to the draws kicked after it.

## Tests

- `gfx_texture_watch_tests` (new, registered with ctest and run under the validation layer by `layer\sys\test.ps1`):
  a texture rewritten a page at a time while it is sampled uploads nothing until the kick after the writes, then
  once; written, kicked and drawn in one frame shows the new content; a write after the watch is armed again is not
  lost; equal bytes are hashed, not uploaded; GPU-side writes apply at once; a host that never marks applies at once,
  and a first mark after unmarked writes applies none twice. Its model of a streamed texture (64 KB, 8 uses a frame):
  `kick-ordered 1.00 uploads and 74-147 us a frame, applied at once 8.02 uploads and 645-1284 us a frame` on NVIDIA
  (100 against 724 us on the Radeon).
  That is 8x fewer uploads and 8.7x less texture time a frame. The queue of CPU writes (`cpu_writes_`) and the
  ranges applied (`changed_taken_`) are vectors that keep their capacity, so they stop allocating once they have
  grown to a frame's writes. Allocations were not metered separately.
- `gfx_cp_tests` (`CpuWriteMarks`, new): the command processor marks a kick and a wait the CPU satisfies with the
  count reported so far; the asynchronous renderer queues each new mark once, in order.
- `any360_bench --frames 240 --check bench/expected/city.txt`: passes before and after on NVIDIA and Radeon with the
  same expected file (largest difference 0 of 255 on NVIDIA, 1 on the Radeon): the picture does not change, so the
  expected file is not regenerated.

## Measured

Two executables, alternated 6 launches each (`runs-whole.txt`, `runs-small.txt`, summary with an exact two-sided
Mann-Whitney rank test in `ab-summary.md`): "before" is origin/main 2027254 with only the bench title's semaphore fix,
"after" is this change. `any360_bench --frames 1200`, headless, uncapped.

Whole machine (16 threads, RTX 5070 Ti, other sessions busy), medians of 6:

| | before | after | change | p |
|---|---|---|---|---|
| texture uploads a frame | 8.18 | 1.02 | -87.5% | 0.002 |
| textures, ms a frame | 0.936 | 0.152 | -83.8% | 0.002 |
| renderer's thread, ms a frame | 3.39 | 2.54 | -25.0% | 0.002 |
| watched writes a frame | 99.5 | 63.9 | -35.7% | 0.002 |
| FPS | 295.7 | 394.5 | +33.4% | 0.002 |
| median frame | 3.29 ms | 2.44 ms | -25.9% | 0.002 |
| 10% low | 242.4 FPS | 308.2 FPS | +27.2% | 0.002 |
| 1% low | 218.5 FPS | 270.6 FPS | +23.8% | 0.002 |
| CPU (100 = a core) | 278% | 292% | +5.0% | 0.009 |
| FPS per CPU% | 1.07 | 1.33 | +23.8% | 0.002 |
| GPU (system / batches) | 9% / 8% | 8% / 7% | -11% | 0.002 |
| GPU ms a frame | 0.279 | 0.172 | -38.4% | 0.002 |
| private memory | 1367 MB | 1358 MB | -0.7% | 0.002 |

Small configuration (`--any360_cpu_affinity=0x3 --any360_gpu_device=Radeon`: 2 processors, the Radeon iGPU):

| | before | after | change | p |
|---|---|---|---|---|
| texture uploads a frame | 1.28 | 1.03 | -19.5% | 0.002 |
| textures, ms a frame | 0.281 | 0.250 | -10.9% | 0.49 |
| FPS | 171.2 | 168.9 | -1.3% | 0.94 |
| median frame | 5.22 ms | 5.34 ms | +2.3% | 0.94 |
| 10% low | 151.1 FPS | 137.3 FPS | -9.2% | 1.00 |
| 1% low | 25.3 FPS | 29.2 FPS | +15.4% | 0.39 |
| CPU (100 = a core) | 130% | 133% | +2.3% | 0.70 |
| GPU (system / batches) | 33% / 24% | 33% / 24% | 0 | 0.93 |
| private memory | 1453 MB | 1424 MB | -2.0% | 0.015 |

- On the whole machine the title's main thread and the renderer's thread run at the same time, so the storm was
  there (8 uploads a frame), and removing it frees a quarter of the renderer's thread, the frame's bound. FPS,
  median and both lows improve together, so the lows are not traded away. More frames a second cost slightly
  more CPU in total, but FPS per CPU% is up 24%.
- On two processors the title and the renderer rarely overlap, so the storm was mostly absent before (1.28 uploads
  a frame). Only the uploads and private memory change significantly; FPS and lows show no measurable difference
  (the 1% lows of 18-40 FPS are the two processors' scheduling, in both variants).
- This scene's lows are bounded by draws (1.1 us of the renderer's thread a draw, submit 0.5-0.7 ms a frame) and
  the command processor (0.5 ms a frame). Those are the next costs in the profile.

## For the in-game handler

Textures the CPU rewrites (Crackdown's and Mirror's Edge's streamed textures, glyph atlases, video planes) now pick up
new content at the first kick after the writes, not at the first draw the renderer reaches after them. A write made
before the kick that holds the draws is always visible to them. The only difference is a write made after the draws
were kicked but before the renderer reached them. Before, the renderer could pick it up, half-written; on the console
it would be a race as well. Now it shows at the next kick. To check in game: streamed textures and text (glyph pages)
in Crackdown and Mirror's Edge, and the frame rate where they stream.

## Also fixed: the bench title's hang at exit

`kSemQueued` (frames ready for the render thread) had a limit of 2. When the render thread was two frames behind at
the end, the main thread's final release (the stop signal) went over the limit and was lost. The render thread then
waited forever, and so did the main thread waiting for it. This happened once in this step's first A/B, after the
renderer got faster. The limit is now 3. The "before" build of the A/B is origin/main with only this fix.
