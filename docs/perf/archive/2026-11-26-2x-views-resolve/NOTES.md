# 2026-11-26: colour resolves of a 2x surface straight from its layers

This follows [2026-11-26-2x-views-copies](../2026-11-26-2x-views-copies/NOTES.md). There, a 2x surface drawn as views
(one layer a sample) still had its layers copied into its rows image before each colour resolve: the resolve blitted
the rows (a linear blit to half the height averages a pixel's two sample rows) and then scaled the result by the
exponent bias.

## What the remaining copies were (Mirror's Edge capture, one pass, scale 1)

Traced by reason, in host pixels copied between the layers and the rows:

| Copy | Mpixels | Share |
|---|---|---|
| Depth layers loaded before the 2x scene (the 1x passes wrote the depth rows) | 24.7 | 35% |
| Colour layers copied into the rows for the two tile resolves | 22.1 | 31% |
| Depth layers copied into the rows before the 1x passes (the depth write-back pass reads and writes them) | 15.7 | 22% |
| Colour layers loaded (a depth pass over the same EDRAM wrote the rows) | 7.7 | 11% |

The tile resolves are k_16_16_16_16_FLOAT targets resolved to k_16_16_16_16_FLOAT textures with an exponent bias of -3.

## The change

- A colour resolve of a surface drawn as views averages each host texel's two samples straight from the layers, in
  one compute pass (`views_resolve_shader.cpp`), with the bias:

  `dest = (layer0 + layer1) * 0.5 * 2^bias`.

  The layers are not copied into the rows (a transpose of every texel), nor blitted and scaled after.
- It applies when the layers hold the rectangle: none of the rows the resolve reads has been written outside them
  since they last matched.
- It applies to half-float destinations only: their resolve images take storage writes. Any other format copies
  through the rows as before.
- `ANY360_VIEWS_RESOLVE_ROWS=1` goes back to the copy through the rows (A/B).
- A depth/stencil split of the copies was tried and dropped. Mirror's Edge's 1x passes write depth too, so on this
  capture it saved nothing and made two copies of one (traced).

## What it changes in the image

- **Scale 1: nothing.**
  - Through the rows, a pixel's two sample rows were averaged; from the layers, its two samples are averaged.
  - It is the same arithmetic: 0 pixels differ over the capture's 12 frames, inline and with 4 recording threads plus
    the asynchronous renderer.
- **Above scale 1: each host texel is the average of the two samples at its place.**
  - Through the rows, a pixel's 2 x scale host rows were sample 0's then sample 1's. A 2:1 blit gave its upper host
    rows sample 0 and its lower ones sample 1, averaged with nothing.
  - On the capture at scale 2, frames 6 and 7 through the rows had a bright flash: mean luminance 176.9 and 172.5,
    where scale 1 has 166.5 and 167.1. The halves the exposure measured were not averages.
  - From the layers they match scale 1 (166.6, 167.1).
  - The other 10 frames are identical, or differ in under 200 pixels.
  - See `frame6_s1_prev-s2_new-s2.jpg`: scale 1 | scale 2 through the rows | scale 2 from the layers.

## Checks

- `gfx_render_tests`, "2x k_16_16_16_16_FLOAT resolve, bias -3, from the layers". A full quad, then a sloped triangle,
  in two half-float colours whose averages are half floats too; resolved with bias -3, both ways.
  - At scale 1, 0 of 4096 words differ from the copy through the rows, on NVIDIA and on the Radeon.
  - Every word is the CPU reference: 2026 of one colour, 1853 of the other, 217 averaged along the edges.
  - The resolve from the layers is counted, and nothing is copied into the rows.
  - At scale 2, every texel away from the edges is the scale-1 word (3176 checked).
- Whole suite (`layer\sys\test.ps1 -Gpus NVIDIA,Radeon`): see the commit.

## GPU time, AMD Radeon iGPU

`gfx_replay --bench --repeat 4` on `elev_s1_multi.cap`, 3 rounds. The figures are ms summed over the run (see
`bench.txt`); the rounds agree within 1%.

| Setting | Rows (old 2x path) | Views, ae3c927 | Views, this change |
|---|---|---|---|
| Scale 1: all GPU work | 305.8 | 435.3 | 421.7 |
| Scale 1: MSAA resolves (layer copies) | 0 | 63.9 | 52.8 |
| Scale 1: resolves | 51.2 | 55.7 | 52.1 |
| Scale 2, host MSAA 4x: all GPU work | 1626.6 | 1999.1 | 1899.1 |
| Scale 2, host MSAA 4x: MSAA resolves (layer copies) | 84.3 | 401.6 | 313.2 |
| Scale 2, host MSAA 4x: resolves | 271.9 | 265.7 | 251.5 |

- The copies into the rows drop from 147 to 51 a run. The pixels copied drop from 280 to 192 M (scale 1) and from
  1122 to 768 M (scale 2).
- All GPU work drops 3.1% at scale 1 and 5.0% at scale 2.
- What remains is depth: the layers loaded after the 1x passes, and copied back before them.
  - Of those 1x passes, the last of a frame is a single rectangle over the whole surface that writes depth and
    stencil with ALWAYS (a clear drawn as a quad, as the XDK clears).
  - Recognised as a clear and applied to both the layers and the rows, it would remove the load (35% of the pixels).
  - That needs the rectangle's coverage and its depth value proven exact. It is the next step on this path.
