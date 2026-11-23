# 2026-11-23: k_16_16_16_16 resolves on the GPU

Item 1 of [the Mirror's Edge 1440p list](../2026-11-23-mirrorsedge-1440p-profile/NOTES.md): resolves of a
k_16_16_16_16 colour target (EDRAM format 5, host R16G16B16A16_SNORM) to k_16_16_16_16 textures (format 26) were
done on the CPU (read back, `lround` texel by texel, written to guest memory, uploaded again). They are now a compute
pass (`layer/gfx/src/vulkan/fixed16_shader.cpp`, `VulkanRenderer::Fixed16Pass`) writing an R16G16B16A16_UNORM GPU
source, when the device stores that format (`shaderStorageImageExtendedFormats`); the CPU path stays for devices
without it.

## Correctness

`gfx_render_tests` "k_16_16_16_16 resolves on the GPU": a format 5 target holding each pixel's position, resolved at
1x and 2x MSAA with exponent biases -6, 0 and +1, every word compared with the CPU path's formula: **0 of 73728
differ**, 6 resolves on the GPU, on the RTX 5070 Ti and the Radeon iGPU (the suite with `-Gpus NVIDIA,Radeon`, 0
validation messages). The older test of this resolve (bias -4) still passes.

## The function: one k_16_16_16_16 resolve on the renderer's thread

Replay of `npc_rb.a360cap` (Mirror's Edge, `gfx_replay --scale 2 --msaa 4 --bench`, 9 frames, 14 format-26 resolves
every 3 frames), three runs each, [replay_bench.txt](replay_bench.txt):

| | Before (CPU) | After (GPU) |
|---|---|---|
| Resolves, renderer thread, 9 frames | 40.4-41.2 ms | 3.4-3.8 ms |
| One format-26 resolve | ~0.89 ms | a few us (recording a dispatch) |
| Host memory a call | 1 Vulkan read-back buffer (up to ~4 MB) + a 512 KB vector: 2 allocations | 0 allocations (the destination image is the resolve's GPU source, reused) |
| Submit, 9 frames | 18.5-20.4 ms | 13.4-15.7 ms |
| Kicks, CPU a frame | 23.3 ms | 12.7 ms |
| GPU a frame | 4.82 ms | 4.83 ms |
| Pixels off vs the ReXGlue reference | 15.0-15.8 % | 15.3-16.1 % (every host pixel kept at scale 2, the CPU path kept the top-left one) |

## In game

Mirror's Edge, `--any360_gpu_scale=2 --any360_gpu_msaa=4 --any360_gpu_vblank_hz=1000`, cores 0-7, RTX 5070 Ti, first
level. Before: `mirrorsedge_199.log` (the profile run of the list); after: `mirrorsedge_200.log`. Periods with more
than 1000 draws a frame (gameplay), medians of the report's periods without diagnostics,
[game_log_compare.txt](game_log_compare.txt), the after run's lines in
[game_log_stats_after.txt](game_log_stats_after.txt):

| | Before (64 periods) | After (14 periods) |
|---|---|---|
| FPS | 51.1 (34-40 in the profiled stretch) | **62.0** (flat: the game's own cap, likely Unreal Engine 3's frame smoothing at 62) |
| Median frame | 17.89 ms | 16.12 ms |
| 10% low | 36.4 FPS | 57.0 FPS |
| 1% low | 30.5 FPS | 48.9 FPS |
| Slowest frame (median of periods) | 42.5 ms | 31.9 ms |
| Resolves, renderer thread a frame | 15.1 ms | 1.2 ms |
| Recording draws a frame | 4.3 ms | 3.1 ms |
| GPU a frame | 4.76 ms | 4.26 ms |
| Buffers created a frame | 9.8 | 0.0 |
| Images created a frame | 2.0 | 2.0 (item 3 of the list) |
| Process private memory | 4705 MB | 4957 MB |

The game now runs at its own cap, so the frame rate gain is bounded by it; the lows are the real measure. One launch
each, not the 5-launch A/B: the periods do not overlap (1% low 7.9-35.8 before, 44.3-53.6 after). Private memory is
higher by ~250 MB (different stretches of the level, and the resolves' GPU sources now kept as images instead of
guest words); not yet separated.

[profile_menus_after.txt](profile_menus_after.txt): the after run's profiler window (90-120 s) fell in the menus,
kept only as a record; it is not compared.
