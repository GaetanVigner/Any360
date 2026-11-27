# any360_bench: Any360's own benchmark title

`bench/` is an original program that runs on Any360 the way a recompiled Xbox 360 title runs on it. It holds no
game code and no game data: geometry, textures, sounds and files are generated from seeds. It exists to measure and
optimise Any360's generic subsystems on a workload that repeats exactly, without launching a game.

## How it binds

- It is its own host on `any360::sys`; no ReXGlue runtime runs. Its guest address space is laid out as a title's
  host lays it out: 4 GB reserved, the virtual part committed on first touch, the 512 MB of physical memory behind
  the three physical views (0xA0000000, 0xC0000000, 0xE0000000 with its 0x1000 offset), and a fourth view for the GPU
  side. CPU writes to memory the texture cache watches are caught by page protection and reported to the renderer,
  as the runtime's physical memory callbacks report them.
- The title code is written in the recompiled form, functions of `(PPCContext&, uint8_t* base)` at guest addresses
  (0x82000000 on). The scheduler runs them through its dispatcher, so the title's threads are `any360::sys` threads
  with their KTHREAD, PCR and stack in guest memory.
- Every system call goes through `any360::sys::kExports` with its arguments in r3 onwards, as a title's import thunk
  makes it: memory (`NtAllocateVirtualMemory`, `MmAllocatePhysicalMemoryEx`), threads (`ExCreateThread`), events,
  semaphores, critical sections and waits, `Vd*` (engines, ring buffer, read pointer write-back, interrupt
  callback, `VdSwap`) and `XAudio*`.
- Graphics: the title writes its frame's PM4 into a command buffer in physical memory the way an XDK D3D runtime does
  (registers, constants, fetch constants, indexed draws, resolves), puts an `INDIRECT_BUFFER`, `VdSwap`'s packet and
  a fence into the ring and kicks it through `CP_RB_WPTR`. any360::gfx's command processor is the `GpuBackend`, in
  front of the Vulkan renderer through the asynchronous renderer, as titles run by default.
- What transfers to games: everything on Any360's side of the exports and the ring. The title code is native C++,
  not recompiled PowerPC, so the cost of translated instructions is not measured here.

## Scenes

| Scene | What it runs | State |
|---|---|---|
| `city` | A grid of procedural buildings (2,000 objects by default, about 1,300 visible a frame plus 512 in a minimap pass), 16 textures (8 DXT1, 4 DXT5, 4 8888, all tiled), one rewritten by the CPU every frame and a DXT1 one every 60 frames (watched writes), a 256x256 minimap pass resolved to a texture and sampled by a billboard, a depth-tested 1280x720 main pass resolved to the front buffer with colour and depth clears, `VdSwap`. Two animation workers driven by events, a render thread fed through semaphores, a critical section, allocation churn. Audio: a render driver client mixing procedural voices, 6 channels, at the audio clock | done |
| `msaa` | the city at 2x/4x MSAA with predicated tiling (2 and 3 bands), depth resolve, occlusion queries | planned |
| `hdr` | k_2_10_10_10_FLOAT / 16_16_16_16 targets with exponent bias, mip chains, bloom-like resolve chains | planned |
| `jobs` | main, render and worker threads with job queues, spin locks, `Sleep(0)` pacing, heap churn | planned |
| `stream` | a procedural pack file streamed with overlapped `NtReadFile` into textures | planned |
| `xma` | XMA contexts decoding procedural streams (needs an XMA2 encoder of its own), 32 voices | planned |
| `records` | the city through the native D3D records (0x7E) instead of PM4 | planned |

## Running it

```
bench\build.ps1                       # out\bench\bench\any360_bench.exe
any360_bench --frames 1200            # headless, uncapped
any360_bench --window --frames 0      # in a window (frames 0: until closed)
any360_bench --gpu Radeon --objects 4000 --workers 4 --json run.json
any360_bench --any360_cpu_affinity=0x3 --any360_gpu_device=Radeon   # the small configuration
```

Options: `--scene city`, `--any360_cpu_affinity=MASK` (kept on those processors, as the titles' option),
`--any360_gpu_device=NAME` (as `--gpu`), `--frames N`, `--warmup N` (frames before the measure, 120), `--objects N`, `--workers N`
(1-4), `--window`, `--gpu NAME`, `--sync-renderer` (no asynchronous renderer), `--sync-pipelines` (every pipeline
compiled before its draw), `--report-seconds S`, `--json FILE`, `--check FILE`, `--write-expected FILE`.

## The report

- Every period (5 s): the two lines titles write, so `tools/frame_lows.py` reads a bench run as it reads a game's:
  `Any360: <fps> FPS; per frame: <n> draws, ...` and `Any360: <fps> FPS with the diagnostics off (bench); frame times:
  median ..., 10% low ..., 1% low ...; slowest frame ...; <n> MB private; CPU <n>% of a core, <n>% of the <n> allowed
  (mask ...); GPU <n>% (system), <n>% (batches)`, the usage text every runtime writes (`any360/host/usage.h`), which
  `tools/frame_lows.py` turns into CPU%, GPU% and FPS per CPU% and per GPU%.
- At the end, over the measured frames: FPS, median, 10% and 1% lows, slowest frame; the same usage (CPU% of a core and
  of the allowed processors, GPU% as the system counts it and as the renderer's batches measure it), FPS per CPU% and per GPU%, private memory; ms a frame of
  the title's main and render threads (their waits excluded), the command processor, the renderer's thread, the
  renderer's draws (textures, shaders, pipelines, guest memory, submit), resolves, swaps and the GPU; per frame the
  draws, resolves, texture uploads, pipelines, system calls and watched writes; the hashes of the command stream and
  the audio. `--json` writes the same figures.
- A/B runs follow `docs/perf/` and the `any360-perf-step` skill: 5 or more runs a variant, alternating, the same
  options, on the resources the session was given, and also on the small configuration (2 cores, the Radeon iGPU).

## Tests

`any360_bench --frames 240 --check bench/expected/city.txt` runs in `layer\sys\test.ps1`, and again under the Vulkan
validation layer on each GPU given (0 validation messages). It checks that every frame was swapped and no draw failed,
that the title is deterministic (the command stream's and the audio's hashes), that the CPU's texture writes reached
the renderer through the write watch, that the last frame matches the expected picture (a 32x18 thumbnail, 12 of 255
at most apart: GPUs filter differently) and that the process's memory stays bounded.
`--write-expected FILE` writes a scene's expected values after a change that is meant to change them.
