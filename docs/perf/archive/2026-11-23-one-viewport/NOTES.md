# 2026-11-23: one host viewport for every draw, partial resolve sources, fixed occlusion counts as an option

## The changes

- **One viewport size for every draw** (`VulkanRenderer::Draw`, `viewport_extent_`): the guest's window coordinates
  were mapped through a host viewport the size of the bound targets. A depth pre-pass (depth target only, 2048 rows)
  and the colour pass after it (EQUAL / GEQUAL, a 1024-row colour target bound) rounded the same vertices differently,
  so their depths differed in the last bits and the colour pass failed in flickering dotted lines over large slanted
  surfaces (Mirror's Edge; very likely Call of Duty: Black Ops's dotted artifacts too). Now the viewport is the
  largest power of two the device takes, at most 16384, for every draw.
- **Partial resolve sources** (`TextureCache`): a texture larger than the last resolve at its address (Mirror's Edge
  resolves shadow maps of 64 to 864 rows to one address and samples an 864x864 texture there) copies the resolve's
  rows from the GPU and keeps its own rows below, but only over a texture that already holds content (a new one is
  read from memory as before).
- **`SetOcclusionCounts(false)` / `GraphicsOptions::occlusion_counts`**: occlusion queries answered by the command
  processor with its fixed count (every object visible, as the reference runtime does) instead of counted on the GPU.
  Default unchanged (counted: Crackdown hides its light flares behind walls with them). Mirror's Edge switches it off:
  with real counts the game's culling makes small models flicker (in a capture, 39 of 307 queries flip between
  visible and zero samples from one frame to the next; the cause is not found yet; with fixed counts the owner sees
  no flicker).

## Tests

- `gfx_render_tests` "depth pre-pass then EQUAL with a shorter colour target": 400 slanted triangles whose corners
  sit within 1/8192 pixel of the rasterizer's subpixel mid-points, a depth-only pre-pass, then an EQUAL colour pass
  with a shorter colour target bound. Old behaviour (viewport sized to the targets): **2085 of 4096** pixels pass;
  now **4096 of 4096**.
- `gfx_render_tests` "occlusion counts off": a query over a draw that passes nothing reads Total 1000, ZPass 1000
  with the counts switched off; the counted test beside it still reads 4096 and 0.
- Whole suite (`layer\sys\test.ps1 -Gpus NVIDIA,Radeon`): 0 failing, 0 validation messages on both GPUs.

## Replays, before / after

Before: the tree of 2026-11-21 20:25 (before 2cc3179's texture bands); after: this change. Pixels off the reference
frame and mean difference per channel ([replays_before_after.txt](replays_before_after.txt), NVIDIA RTX 5070 Ti):

| Capture | Before | After |
|---|---|---|
| Mirror's Edge npc_rb (ReXGlue D3D12 reference) | 20.5 / 24.0 / 23.1 % | 10.7 / 10.9 / 10.9 % |
| Mirror's Edge npc_ref | 88.2 / 96.7 / 97.5 % | 87.0 / 97.1 / 97.5 % (reference without resolve readback) |
| Black Ops spawn3, lamp, debris, black | 1.10, 2.72, 0.77, 1.67 % | unchanged |
| TDU drive, t1 | no reference frames; every draw drawn | unchanged |

GPU time a frame (`gfx_replay --bench`, cores 0-7, two runs each, [bench.txt](bench.txt)):

| Capture | Before | After |
|---|---|---|
| Mirror's Edge npc_rb | 5.019, 5.007 ms | 4.903, 4.820 ms (texture copies 2.61 -> 2.49, write-backs 0.18 -> 0.05) |
| Black Ops spawn3 | 5.358, 5.357 ms | 5.769, 5.535 ms (other 0.23 -> 0.31, depth resolves 0.15 -> 0.21: the bands of 2cc3179) |

Host memory: no new allocation a draw (one `uint32_t` member; the partial-source lookup is one map find a texture
bind that has no full source).
