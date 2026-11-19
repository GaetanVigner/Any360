# 2026-11-19: D3DResource_GetType, the GPU format's block and DrawVerticesUP native (D1)

Three entries of Crackdown's XDK D3D runtime the 2026-11-18 route count found called and not covered:

- `sub_826ACDB0`, D3DResource_GetType: the type in the resource's first word, a texture's refined by its dimension
  (the word at +48, bits 9-10: line 20, 2D 3 or array 19 by bit 10 of +32, volume 17, cube 18), a surface (4) that
  holds a parent whose dimension is volume: a volume (16). `any360::xdk::D3dResourceType` (`d3d.h`), host kind
  `xdk::d3d_state::ResourceType`.
- `sub_826ACFE8`, a GPU texture format's block in texels (4x4 for the block-compressed formats 18-20, 49, 51-53,
  58-61; 4x1 for 39 and 44; 2x1 for 11, 12 and 40; 1x1 otherwise), the width to *r4 and then the height to *r5.
  `any360::xdk::D3dFormatBlock` (a 64-entry table), host kind `xdk::d3d_state::FormatBlock`.
- `sub_826B9688`, DrawVerticesUP (DrawPrimitiveUP's body): BeginVertices (`sub_826B91E0`), the vertices copied into
  the block it returns, EndVertices (the cursor at +48 moved to +13076). Host kind `xdk::d3d_state::DrawVerticesUp`:
  it calls the title's BeginVertices entry (`g_begin_vertices`, so the title's hooks and Any360's native
  BeginVertices run as before) from the original's 128-byte frame and copies with the CRT memcpy's native
  (`xdk::CrtMemcpy`), the title's memcpy (`g_vertex_copy`) when the copy is one that native declines. Declined in
  the verify and bench modes (BeginVertices takes room in the command buffer: the draw would be sent twice).

Their instruction-by-instruction references: `d3d_reference.h`, `reference::resource::GetType` and `FormatBlock`.

## Tests

- Unit test (`xdk_d3d_tests`, `ResourceTypesAndBlocks`): GetType on 200,000 random resources (mostly textures and
  surfaces, every dimension, the array bit, a surface's parent bit and its parent's dimension) against the reference,
  every type it can give reached; its declines (a word not plain) read nothing; the format block on every number up
  to 300 and 19,700 random 32-bit ones, the width and height words apart, the same word or overlapping, the bytes
  around them compared. Time a call: GetType 1.04 ns (reference 0.84 ns: the reference has no memory checks), block 0.22 ns (reference 1.92 ns);
  0 bytes and 0 allocations (`unit_test.txt`).
- In-game self-test (3 launches each, `selftest_*.txt`), the title's code against the native on random inputs:

| function | calls | different | original (median) | native (median) | speed | memory a call (original / native) |
|---|---:|---:|---:|---:|---:|---|
| D3DResource_GetType | 3 x 3000 | 0 | 6.3-7.5 ns | 5.0-6.3 ns | 1.20-1.50x | 0 B, 0 allocations / 0 B, 0 allocations |
| GPU format block | 3 x 3000 | 0 | 7.5-10.0 ns | 3.8-5.0 ns | 1.75-2.00x | 0 B, 0 allocations / 0 B, 0 allocations |
| DrawVerticesUP | 3 x 3000 | 0 | 110.0-118.8 ns | 103.8-112.5 ns | 1.06x | 0 B, 0 allocations / 0 B, 0 allocations |

  DrawVerticesUP's time is mostly BeginVertices and the copy, which both sides run the same way (the title's bound
  entries); its gain is the call's own frame, the memcpy's call and EndVertices.
- Verify mode in the city (`--any360_xdk_verify=true`, 2 launches of 150 s, `verify_*.txt`): GetType 192,885 and
  196,095 calls, the format block 124,594 and 126,617 calls, 0 different; DrawVerticesUP 360,251 and 367,120 calls,
  all left to the title's code (declined, as above).

## In game

Bound, all three. 5 v 5 launches of 150 s on the Radeon (cores 8-15), alternating, after first, the same route
(`ab_rtype_runs.txt`, `ab_rtype_off_runs.txt` and their `*_lows.txt`, tools/frame_lows.py):

| | before (records on) | after (records on) | before (records off) | after (records off) |
|---|---:|---:|---:|---:|
| FPS (steady periods, mean / median) | 62.9 / 63.0 | 62.8 / 62.8 | 63.1 / 63.1 | 63.1 / 63.1 |
| 10% low | 62.1 | 62.0 | 62.6 | 62.7 |
| 1% low (mean / median) | 56.8 / 60.7 | 58.3 / 61.4 | 61.8 / 61.8 | 61.8 / 61.8 |
| private memory | 2587 MB | 2579 MB | 2586 MB | 2585 MB |

No difference: rank z between -1.74 and +0.58 with records on, between -0.94 and +1.07 with records off (the three
together are about 680,000 calls a route; at the self-test's gains they save about 3 ms of CPU time in 150 s). With records on every launch of
both arms has the records mode's known stretches of 35 FPS frames. Launches 083 and 084 overlapped another session's
6-minute CPU bench on cores 0-7; they are in line with the other pairs.
