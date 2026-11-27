# The renderer's batch rings grown on demand (2026-11-28)

**What held the memory.** The Simpsons Game on Any360 took about 1.1 GB more private memory than on ReXGlue (2800 MB
against 1703 MB, 2026-11-26 benchmark). One launch a variant, the process's private allocations listed at 100 s
(`bisect-regions.txt`, by VirtualQueryEx):

| variant | private committed |
|---|---:|
| ReXGlue (runtime GPU, input group only) | 1365 MB |
| `any360::sys` (8 groups) and the XDK natives, runtime GPU | 1259 MB |
| `any360::gfx` only | 2375 MB |
| all of Any360 | 2455 MB |

The renderer holds it. `ANY360_LOG_ALLOCATIONS=1` (now logging from the device's creation, with the private memory
at each step of the start; `start-allocations-before.txt`): 750 MB private with the device and the renderer built,
1790 MB once started. Of the 1040 MB:

- 512 MB: guest memory's copy in video memory (`renderer.cpp`, `vram_guest_`), device-local but counted as private
  memory by the NVIDIA driver;
- 384 MB: each of the 8 batches' rings, 32 MB of constants and 16 MB of indices, host-visible;
- about 150 MB: the rest (descriptor pools, command pools, small buffers).

ReXGlue's own GPU starts in both variants (its 512 MB shared memory is in the 714 MB before Any360's device).

**Change.** Each batch's rings start at 2 MB of constants and 1 MB of indices. A draw that finds no room raises the
capacity (doubled, or the draw's need rounded up to a power of two, up to the old 32 MB / 16 MB) and flushes; each
batch takes the new capacity when it is next free (its buffers replaced, its constant descriptor set rewritten).
`gfx_render_tests` "ring growth" draws 12 x 128 KB of DMA indices into one batch: the index ring grows, every draw
lands, nothing fails. `RendererStats::ring_growths` counts the growths.

**Measured** (Any360 38dfaac before, 38dfaac + this change after; same build otherwise; The Simpsons Game, title
screen into the first level, uncapped, 300 s a launch, 5 v 5 alternating, `frame_lows.py --after 150`):

| profile | | FPS mean (median) | 10% low | 1% low | private MB (mean / max) | CPU% of a core | GPU% |
|---|---|---|---:|---:|---:|---:|---:|
| usual: `--any360_cpu_affinity=0xFF`, RTX 5070 Ti | before | 770.2 (765.2) | 668.1 | 532.8 | 2748 / 2777 | 273 | 56 |
| | after | 769.3 (768.4) | 664.7 | 528.4 | **2408 / 2420** | 271 | 56 |
| small: `--any360_cpu_affinity=0x3`, Radeon iGPU | before | 94.9 (95.0) | 93.2 | 85.9 | 2761 / 2780 | 58 | 100 |
| | after | 94.9 (95.0) | 93.1 | 87.1 | **2432 / 2460** | 58 | 100 |

Rank tests (before against after): usual FPS z +0.99, 10% low +1.65, 1% low +1.97, dips' 1% low +0.10; small FPS
-0.40, 10% low +0.34, 1% low -0.95. No difference in frame rate or lows; 340 MB (usual) and 329 MB (small) less
private memory. The small profile is GPU-bound (100%).

**Left.** The 512 MB copy of guest memory in video memory is the next largest: a sparse buffer committing only the
pages draws read would keep most of it out. Any360 is still about 700 MB above ReXGlue.

Files: `runs-*.txt` (the launches), `lows-*.txt` (`frame_lows.py`), `ab-output.txt` (the launch script's output),
`bisect-regions.txt`, `start-allocations-before.txt`. Suite: `layer\sys\test.ps1 -Gpus NVIDIA,Radeon`, 0 failing
checks, 0 validation messages.
