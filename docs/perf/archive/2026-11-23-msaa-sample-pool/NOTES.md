# 2026-11-23: host MSAA samples pooled

Item 3 of [the Mirror's Edge 1440p list](../2026-11-23-mirrorsedge-1440p-profile/NOTES.md): 9-10 buffers and 2 images
created a frame (320-350 MB). After the GPU resolves (item 1) the buffers were gone; the images remained. The
allocation log (`ANY360_LOG_ALLOCATIONS=1`, now in `gfx_replay` too: [allocations_before.txt](allocations_before.txt))
named them: a 2560x2048 R16G16B16A16_SFLOAT image with 4 samples (160 MB), created at `CreateSamples` every frame. A
surface drawn in a pass mixing one-sample and multisampled targets (or a depth target drawn alone through a frame)
goes back to one sample (`DropSamples`: its samples resolved, its multisampled image freed after the batch); a resolve
clearing it whole gives it samples again (`GiveSamplesBack`): a 160 MB image created and freed a frame.

Now the dropped image (with its integer view) goes into a pool once the batch that last used it has run
(`FinishBatch`), and `CreateSamples` takes one of the same size, format and sample count before creating one; an image
unused for 2 s is freed (`FreeSpareSamples` at each swap). `ANY360_NO_SAMPLE_POOL=1` switches the pool off (the A/B
below). Generic: any title with host MSAA whose targets go back to one sample gets it.

## Unit test

`gfx_render_tests` "host MSAA samples pooled" (4x host MSAA): a depth target drawn alone through a frame dropped at the
swap, a colour target dropped in a mixed pass; two new targets of their sizes and formats take both images from the
pool (2 reused, 0 for the next targets once the pool is empty), and what they draw is the same as with fresh samples
(0 of 16384 bytes differ). Passes on NVIDIA and Radeon under the validation layer.

## The function: CreateSamples for a 2560x2048 4x RGBA16F surface

Replay of `npc_rb.a360cap` (`gfx_replay --scale 2 --msaa 4 --bench --repeat 3`), same binary, pool off / on,
alternating, 3 rounds each ([replay_ab.txt](replay_ab.txt)):

| | Pool off | Pool on |
|---|---|---|
| Kicks, CPU a frame (last pass) | 9.54-9.95 ms | 7.67-8.06 ms |
| A call that needs samples again | ~2 ms: `vkCreateImage` + `vkAllocateMemory` (160 MB of video memory) + view, then the image, view and memory freed after its batch | a vector scan under a mutex: no Vulkan object, no allocation |
| Images created over 3 passes | 9 x 160 MB (+ the others, unchanged) | 0 x 160 MB (8 of 8 given back from the pool) |
| GPU a frame | 4.80-4.81 ms | 4.80-4.82 ms |
| Frames vs the reference | the same | the same |

## In game

Mirror's Edge, 1440p host MSAA 4x, first level, about a minute of play each, the profile started with F6
([game_log_compare.txt](game_log_compare.txt): the first two are the runs of the D3D wait step, before this change):

| | Before (2 runs) | Pool |
|---|---|---|
| Images created a frame | 2.00-2.01 (320 MB) | 0.00 (0.08 MB) |
| Process CPU (profile) | 1.70 cores | 1.58 cores |
| Driver CPU (`vkGetInstanceProcAddr`, the driver's code), 30 s | 3.19 s | 2.76 s |
| Resolves, renderer thread a frame | 1.12-1.19 ms | 0.87 ms |
| FPS / 10% low / 1% low | 62 / 57-58 / 49-51 | 62 / 58 / 51 |
| Private memory | 4988-5036 MB | 4956 MB |

The game stays at its 62 FPS cap; the gain is CPU (renderer thread and driver) and no 320 MB of video memory
allocated and freed a frame. Short single runs: an in-game check, not a frame rate A/B.
