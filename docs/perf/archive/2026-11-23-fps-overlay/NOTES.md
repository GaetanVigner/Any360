# The frame counter's cost a frame, on and off (2026-11-23)

`--any360_fps_overlay=true` (docs/GFX.md, "The frame counter, for every title"). Measured with
`gfx_overlay_tests NVIDIA --bench` on cores 0-7 and the RTX 5070 Ti (raw output: `bench_nvidia.txt`).

CPU: the binding's per-frame work (the frame time into the 512-frame window; twice a second the summary, the text and
the band rasterized for each of the presenter's three frame slots), amortized over 2 M frames:

| Case | Off: ns a frame | On: ns a frame | On: allocations a frame | On: bytes a frame |
|---|---:|---:|---:|---:|
| 60 FPS, 1080p window | 0.6 | 532 | 0.033 (one text string a refresh) | 3.2 |
| 240 FPS, 1440p window | 0.6 | 181 | 0.008 | 0.8 |

Off is the branch alone (the binding does not call its counter, the presenter records nothing for an empty text).
Memory on: the window 4 KB, the three host-visible bands 0.35 MB (1080p and 1440p: font scale 2), made once.

GPU: the presenter's copy of the band onto the swapchain image, timed with timestamps over 240 copies:

| Window | Band | Off | On, a frame |
|---|---|---:|---:|
| 1920x1080 | 496x50, 97 KB | 0 (nothing recorded) | 2.1 us |
| 2560x1440 | 496x50, 97 KB | 0 (nothing recorded) | 2.1 us |
| 1920x1080 / 2560x1440, AMD Radeon iGPU (`bench_radeon.txt`) | 496x50, 97 KB | 0 | 7.4-7.5 us |

At 240 FPS (4.2 ms a frame) the copy is 0.05% of a frame on the RTX, 0.18% on the iGPU, the CPU side 0.004%. A first scale (one font pixel per 360
lines, 3 at 1080p, 4 at 1440p) was found too big on screen by the owner; at that size the copy cost 4-5.5 us (1080p)
and 15.5-17.6 us (1440p, a 388 KB band), the CPU side 359-710 ns. The first version of the overlay
waited for the other two frames in flight each time the text changed (twice a second); the per-slot bands remove that
wait, which could show in the 1% low.
