# 2026-11-26: guest 2x MSAA drawn at the console's sample positions (views), its depth resolved at the host's resolution

## The defect

Mirror's Edge's elevator walls were black at the campaign's start (scale 1). At scale 2 the walls were in column
stripes, and characters looked dithered, as if seen through.

- The title draws its scene at 2x MSAA with the viewport moved by sample 0's offset (offsets 639.75 / 360.25 against
  640 / 360 in its 1x passes).
- It resolves sample 0's depth, writes it back through a 1x pass (oDepth), and tests its lights GEQUAL against it.
- That only works when the depth matches the 1x pass to the bit.

Two mismatches broke it:

- **Scale 1, the sample's position.** Any360 drew a 2x surface as two host rows a pixel, both samples at the pixel's
  x. Sample 0 then saw the scene a quarter pixel away from where the console's sample does, and the lights were
  depth-rejected.
  - The position was measured on a replayed capture with an x/y offset sweep: x -0.25, a step (0.125 fails, 0.375 is
    worse).
  - The y offset comes from the title's registers: sample 0 is at (-0.25, +0.25) in window coordinates (y down).
  - Sample 1 is taken at (+0.25, -0.25), by symmetry; it is not measured.
- **Scale 2, the resolution.** Depth resolves were kept at the guest's resolution. The restored depth was then one
  value for 2x2 host pixels, and on any slope one host column in two failed GEQUAL.

## The change

- **2x as views** (`VulkanRenderer::UseViews`, `Surface::views`):
  - A 2x target gets an array image, one layer a sample, drawn by multiview passes (`viewMask` 0b11).
  - Each view has its own NDC offset: `ndc_offset` for sample 0, `ndc_offset_view1` for sample 1, chosen by
    `gl_ViewIndex` in the translated vertex shader. The offsets are sums of quarters, exact in float.
  - The pixel shader's parameter generation and alpha to coverage take the sample from the view
    (`kSysFlag_MsaaSamplesAsViews`).
  - The rows image stays the surface everything else reads: EDRAM aliasing, resolves, read-backs and the presenter.
  - `CopyViews` copies the layers into the rows before those reads (`ResolveSamples`). It loads the rows into the
    layers before a 2x pass when the rows were written meanwhile (`views_stale`).
  - Within a batch the copy goes through a buffer, with a compute pass reordering the rows (`view_rows_shader.cpp`).
    Outside one (read-backs, the presenter) it is a copy a pixel row's sample.
  - Host MSAA is not applied over a 2x pass: its two samples are the console's.
  - `ANY360_2X_AS_ROWS=1` switches back to the rows path, for A/B.
- **Depth resolves from views** (`ResolveDepthOnGpu`):
  - The depth is read from the resolved sample's layer at the host's resolution.
  - The written-back words take the top-left host texel of each guest texel, as a resolve at the guest's resolution
    reads its pixel.
  - Depth resolves of other surfaces (shadow maps) stay at the guest's resolution, as Call of Duty: Black Ops needs.

## Tests

- `gfx_render_tests` "2x sample positions" draws a plane sloped along x and y three ways:
  - at 1x;
  - at 2x, moved by sample 0's offset, then sample 0 resolved;
  - at 2x, moved by sample 1's offset, then sample 1 resolved.
- Every depth word must equal the 1x one, at scales 1 and 2:
  - rows path: 4096 of 4096 words differ at scale 1, 4032 at scale 2;
  - views path: 0 and 0.
- The same test checks that the layers are loaded once and that no copy took the per-row path.
- Whole suite (`layer\sys\test.ps1 -Gpus NVIDIA,Radeon`): 0 failing, 0 validation messages on both GPUs.

## In game (Mirror's Edge, route2 from launch, uncapped vblank, 5 v 5 alternating, RTX 5070 Ti)

| Setting | Variant | FPS | 10% low | 1% low | Private MB | GPU ms a frame, MSAA resolves (view copies) |
|---|---|---|---|---|---|---|
| 720p (defaults) | views | 61.9 | 58.5 | 54.1 | 3963 | 0.99 |
| 720p (defaults) | rows | 61.9 | 58.1 | 52.6 | 3728 | 0.00 |
| 1440p, host MSAA 4x | views | 61.9 | 58.3 | 52.4 | 5026 | 4.62 |
| 1440p, host MSAA 4x | rows | 61.9 | 58.4 | 54.0 | 4871 | 0.13 |

- Rank tests (`*-lows.txt`): 720p 10% low z +2.42, everything else |z| <= 1.2. Both settings are at the game's own
  62 FPS cap either way.
- The cost is GPU time the cap hides on this card: about 1 ms a frame at 720p and 4.6 ms at 1440p of layer copies,
  plus 150-240 MB of private memory (layers and the copy buffer).
- That is the next optimization for small hardware: colour resolves read from the layers as depth resolves do, and
  fewer reloads when a surface alternates between 2x and 1x passes.
- First attempt, per-row copy regions (about 1000 regions a copy): 43.0 FPS, 1% low 32.0 against 61.9 / 54.1, with
  13 ms a frame of GPU in copies. Replaced by the buffer copies before measuring the above.

Owner's check in game: the elevator walls are lit at 720p and at 1440p MSAA 4x, with no stripes, and Kate is no
longer seen through.
