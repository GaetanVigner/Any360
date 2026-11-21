# EDRAM depth under colour, rectangle lists unculled, draw extents from vertices (2026-11-21)

Mirror's Edge's first NPC (Celeste) was half transparent: its light buffer (k_8_8_8_8 at EDRAM 0x2D0, read by the
characters' lighting) must start as the words of a depth 1.0 / stencil FF clear the title draws over the same EDRAM,
and its light-volume stencil needs the depth restore before it. Three generic fixes:

- Depth written over a colour surface's EDRAM is read by that surface as the depth words (depth << 8 | stencil,
  unorm24 or float24), converted on the GPU (depth_word_shader.cpp), only over the rows a draw or resolve reaches and
  not over rows a clear rectangle overwrites whole.
- Rectangle lists are never culled (the reference's draw_util): the title's depth restore is a clockwise rectangle.
- A draw without clipping under Direct3D 9's 8192 scissor writes only the rows its vertices reach: its vertex shader
  runs on the CPU (the reference's interpreter, vendored) as the reference's DrawExtentEstimator does.

## Image against ReXGlue's (gfx_replay, frames_vs_reference.txt; before = 8754acf, after = this change)

| capture | before | after |
|---|---|---|
| Mirror's Edge, Celeste (ReXGlue reference, resolves read back) | 20.5 / 24.0 / 23.1 % off | 10.8 / 10.9 / 11.0 % off |
| Call of Duty, 4 captures | 1.10, 2.72, 0.77, 1.67 % | identical |
| Test Drive Unlimited, 2 captures (no reference) | | byte-identical frames |

## Cost (replay_ab.csv: 5 alternating runs a variant, NVIDIA RTX 5070 Ti, third pass of --bench --repeat 3)

| capture | kicks ms/frame before -> after | GPU ms/frame before -> after |
|---|---|---|
| Mirror's Edge npc_rb | 20.88 -> 20.82 | 1.982 -> 2.062 |
| CoD ref_spawn3 | 18.88 -> 19.59 | 2.148 -> 2.301 |
| CoD ref_lamp | 19.78 -> 19.82 | 1.968 -> 2.148 |

The vertex extent costs 0.9-1.3 us a draw (42 draws a CoD frame pair: 0.04-0.05 ms; 77 Mirror's Edge draws: 0.10 ms),
0 bytes and 0 host allocations a call beyond the interpreter's stack state. The first version converted depth over a
colour surface's whole span (to the end of EDRAM): 7641 conversions and 164 ms a CoD frame; limited to the rows in use
and skipping rows a clear rectangle overwrites, 4 over the two frames.
