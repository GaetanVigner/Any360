# 2026-11-26: fewer and cheaper copies between a 2x surface's layers and its rows

This follows [2026-11-26-2x-msaa-views](../2026-11-26-2x-msaa-views/NOTES.md). There, drawing guest 2x MSAA as views
cost GPU time in copies between the layers and the rows image. NVIDIA's cap hid it; on the AMD Radeon iGPU it was
13.4 ms a frame at 720p and 48.7 ms at 1440p, in game.

## What a frame copied (Mirror's Edge capture, traced)

- The layers were loaded whole before each 2x pass. Most of what was loaded was rows that no 1x pass had written: a 1x
  pass that only tests depth was counted as writing it.
- The layers were copied back whole when a surface went from 2x to 1x passes, rows no 1x pass read included.
- The clear of a resolve went into the layers only. What read the rows later copied the cleared rectangle back.
- Each copy took four passes over the data: image to buffer, a reordering compute pass reading and writing, then
  buffer to image. The compute pass ran one invocation per 4-byte word, with three divisions each.

## The change

- **Stale rows instead of a flag** (`Surface::views_stale[2]`).
  - The layers reload only the rows written outside them since they last matched: 1x draws that write
    (`GrowDrawn`'s `writes`), alias copies and depth words (`DropSamples(surface, top, bottom)`).
  - A depth target that a 1x pass only tests (no depth write, stencil keeping every value) writes nothing.
- **Lazy copy back.** Going to 1x passes copies nothing.
  - A 1x draw copies back the rows it reaches that the layers hold newer, before drawing.
  - Every read is served by `ResolveSamples` as before.
  - A band through the middle of what the layers hold newer copies all of it, so that the two sides never both hold
    newer rows.
- **Clears on both sides.**
  - A resolve's clear of a surface drawn as views clears its rows too, and `ShrinkDrawn` takes the rectangle out of
    what the layers hold newer.
  - A clear made as rows clears the layers too (whole pixel rows), and empties the stale rows it covers.
  - When the rectangle is not whole pixel rows, the layers are copied back first.
- **Cheaper copies.**
  - At scale 1, a layer's rows are every other row of the rows image. The buffer copies' row length does the
    interleave, so there is no compute pass: two passes over the data.
  - Above scale 1, the reordering pass moves 16 bytes an invocation, dispatched by row (rows padded to 16-byte words).

## Checks

- Replay of the reference capture (`elev_s1_multi.cap`, 12 frames) at scales 1 and 2, inline and with 4 recording
  threads plus the asynchronous renderer: **0 pixels differ** from the committed views path (67aff5e), every frame.
- `gfx_render_tests` "2x, 1x depth test, 2x" (scales 1 and 2): a 2x pass, then a 1x pass that only tests its depth,
  then the 2x pass again.
  - The 1x pass copies the layers back once; the second 2x pass loads nothing.
  - Sample 0's depth resolved after it equals the 1x pass's, every word.
- "2x sample positions" still checks 0 words different.
- Whole suite (`layer\sys\test.ps1 -Gpus NVIDIA,Radeon`): 0 failing, 0 validation messages on both GPUs.
- In game (1440p, host MSAA 4x, elevator): walls lit, no stripes.

## GPU time, AMD Radeon iGPU

`gfx_replay --bench --repeat 4` on `elev_s1_multi.cap`. The figures are ms summed over the run, the same run for every
column, so the columns compare. See `bench.txt`.

| Setting | Rows (old 2x path) | Views, 67aff5e | Views, this change |
|---|---|---|---|
| Scale 1: all GPU work | 303 | 529 | 444 |
| Scale 1: layer copies (MSAA resolves) | 0 | 165 | 68 |
| Scale 1: draws | 239 | 297 | 305 |
| Scale 2, host MSAA 4x: all GPU work | 1611 | 2359 | 2000 |
| Scale 2, host MSAA 4x: layer copies | 79 | 803 | 400 |

- The layer copies drop by 59% at scale 1 and 50% at scale 2. The pixels copied drop from 446 to 280 M (scale 1) and
  from 1784 to 1122 M (scale 2).
- What remains:
  - colour resolves read the rows, so the layers are copied back for each tile resolve;
  - a 1x pass writes depth over the whole surface (its extent unknown), so the next 2x pass loads it;
  - a 1x pass tests depth over rows the layers hold newer.
- Multiview itself costs about 25% more in draws at scale 1 on this GPU (vertex work twice; fragment work is the same
  as the rows path). At scale 2 it is under 5%.
- Next on this path: resolves that read the layers directly, and a lower vertex cost for views (or real host 2x MSAA
  at programmable sample positions, once its depth is proven bit-equal).
