# The renderer's cost a draw: targets taken as they were when depth sits below the scissor (2026-11-28)

After the texture-watch step (`../2026-11-28-bench-texture-watch/`), the bench's city scene ran at about 394 FPS on the
whole machine, still bound by the renderer's thread: draws 2.4-2.5 ms a frame for 2,530 draws (about 1 us a draw).

## Where a draw's time went

The draw's four timed parts (`RendererStats::ns_draw_*`, shown for this step by a temporary line in the bench's
summary, not kept), ms a frame, NVIDIA, 1200 frames:

| part | before | after |
|---|---|---|
| setup (shaders, textures, fixed state) | 0.33 | 0.23 |
| targets, scissor and pipeline | 1.05 | 0.09 |
| constants and indices | 0.37 | 0.27 |
| descriptor sets and recording | 0.63 | 0.50 |
| submit (beside the draws) | 0.59 | 0.46 |

"Targets" should be nearly free: the renderer keeps the last draw's targets, scissor and pipeline (`target_memo_`) and
takes them as they are when the registers they come from are the same. Counted with a temporary counter: 73% of the
draws missed it, while the key differed for 0.3% of them. The misses came from `AliasesNewer` on the colour target,
which returned true at every draw because of `depth_over`:

- A colour surface spans from its base to the end of EDRAM (the city's 1280x720 target: an image of 1280x2048), so
  it lies over the depth surface's EDRAM (tile 720 on: its rows 45 and down).
- Every depth-tested draw marks its depth rows written, which set `depth_over` on that colour surface.
- `SyncDepthIntoColour` converts only the rows the draw uses (down to the scissor: an earlier fix for Call of
  Duty: Black Ops, whose targets sit over its depth the same way), then sets `depth_over` again because depth
  remains under the rows below. It also scanned those rows at every draw to find that out.
- So every draw took the slow path: surface lookups, `SyncAliases`, the scan, and the pipeline lookup.

## The change

- `Surface::depth_over` (a flag) becomes `depth_over_row`: the first of the colour surface's tile rows a depth
  surface wrote over since it last looked (`SIZE_MAX`: none). `MarkWritten` lowers it to the first row a depth write
  reaches; `SyncDepthIntoColour` sets it to the first row below the ones it converted that still has newer depth.
- `AliasesNewer(surface, need_bottom)`: depth counts only above `need_bottom` (`DepthOver`), and rows of the group
  as before (`GroupNewer`). The target memo passes the draw's scissor rows (computed before the memo check now);
  `SyncAliases` converts depth only when it lies above the rows the caller needs.
- What a draw converts is unchanged: the same rows, the same conditions. Depth under the rows below is converted
  when a draw, a resolve or a read reaches them, as before.
- `RendererStats::targets_reused` (new): draws that took the last draw's targets as they were.

## Tests

- `gfx_render_tests` "depth below the scissor" (new): colour at tile 0, depth at tile 64 (pixel row 1024 of the colour
  surface at pitch 64), 8 depth-tested draws in a 64-row scissor: 7 of 8 take the targets as they were (0 of 8 before
  this change: checked by building the test against the old memo check), all pixels right, nothing converted for
  the draws, and a colour surface read over those rows still gets the depth words. The scene's surface count goes from 26 to 28 (its two surfaces).
- `any360_bench --frames 240 --check bench/expected/city.txt`: passes before and after (largest difference 0 of 255):
  the picture does not change.
- The suite, NVIDIA and Radeon under the validation layer: green (`layer\sys	est.ps1 -Gpus NVIDIA,Radeon`), 0 validation messages on each GPU.

## Measured

Two executables, alternated 6 launches each (`runs-whole.txt`, `runs-small.txt`, an exact two-sided Mann-Whitney
rank test in `ab-summary.md`): "before" is origin/main 6eec1c3 (which includes the batch rings step, so it already
runs faster than the 394 FPS above), "after" is this change. `any360_bench --frames 1200`, headless, uncapped.
2,530 draws a frame.

Whole machine (16 threads, RTX 5070 Ti, other sessions busy), medians of 6:

| | before | after | change | p |
|---|---|---|---|---|
| draws, ms a frame | 2.04 | 1.20 | -41% | 0.002 |
| us a draw (draws / 2,530) | 0.81 | 0.47 | -41% | 0.002 |
| renderer's thread, ms a frame | 2.18 | 1.35 | -38% | 0.002 |
| FPS | 458 | 741 | +62% | 0.002 |
| median frame | 2.05 ms | 1.25 ms | -39% | 0.002 |
| 10% low | 381 FPS | 607 FPS | +59% | 0.004 |
| 1% low | 300 FPS | 433 FPS | +44% | 0.065 |
| CPU (100 = a core) | 297% | 342% | +15% | 0.058 |
| FPS per CPU% | 1.56 | 2.12 | +36% | 0.002 |
| GPU (system / batches) | 8% / 7% | 13% / 12% | | 0.004 |
| GPU ms a frame | 0.151 | 0.150 | 0 | 0.72 |
| system calls a frame | 9,653 | 5,218 | -46% | 0.002 |
| private memory | 979 MB | 980 MB | 0 | 0.26 |

The 1% low's p of 0.065 comes from the first "after" launch alone: a 150 ms stall with 5 frames over 16.7 ms on the
shared machine (1% low 110 FPS). The other five "after" launches (418-468 FPS) are all above every "before" launch
(267-337 FPS). GPU% rises because more frames a second run; the GPU's time a frame is unchanged. Fewer system calls
a frame: the title's render thread yields less while it waits on a faster renderer.

Small configuration (`--any360_cpu_affinity=0x3 --any360_gpu_device=Radeon`), medians of 6:

| | before | after | change | p |
|---|---|---|---|---|
| draws, ms a frame | 3.81 | 2.42 | -36% | 0.002 |
| us a draw | 1.51 | 0.96 | -36% | 0.002 |
| renderer's thread, ms a frame | 4.12 | 2.76 | -33% | 0.002 |
| FPS | 173 | 215 | +25% | 0.002 |
| median frame | 5.10 ms | 3.80 ms | -26% | 0.002 |
| 10% low | 140 FPS | 170 FPS | +22% | 0.026 |
| 1% low | 33.4 FPS | 33.5 FPS | 0 | 1.00 |
| CPU (100 = a core) | 131% | 141% | +8% | 0.10 |
| FPS per CPU% | 1.36 | 1.51 | +11% | 0.011 |
| GPU (system / batches) | 33% / 24% | 37% / 27% | | 0.006 |
| GPU ms a frame | 1.35 | 1.24 | -8% | 0.002 |
| private memory | 1055 MB | 1070 MB | +1.4% | 0.26 |

On two processors the 1% low stays at about 33 FPS: it comes from the two processors' scheduling between the
title's threads, the command processor and the renderer, in both variants, not from the draws.

Allocations a frame: the bench does not meter them. This change adds none: the memo hit path allocates nothing,
and the miss path it avoids allocated nothing either (lookups in existing maps).

Next in the profile (after): recording and descriptor sets 0.50 ms a frame, submit 0.46, constants 0.27, setup
0.23; the command processor about 0.45 ms a frame (refresh-codbo's).

## For the in-game handler

Any title whose colour targets sit over its depth's EDRAM below their scissor (most do: a colour surface spans to the
end of EDRAM) took the slow path at every depth-tested draw. Expect less time on the renderer's thread a draw in
Crackdown, Mirror's Edge, Call of Duty: Black Ops and Test Drive Unlimited. To check in game: depth under colour
(Mirror's Edge's depth pre-pass and its EQUAL colour pass, Call of Duty's targets over its depth) looks the same.
