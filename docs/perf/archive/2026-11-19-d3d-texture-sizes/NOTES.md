# 2026-11-19: a texture's dimensions and a texture level's bytes native (D1)

Two more entries of Crackdown's XDK D3D runtime from the 2026-11-18 route count:

- `sub_826AD598`, a texture's sizes from its fetch constant: its dimension (+48 bits 9-10) says which fields of the
  word at +36 hold the width, height and depth less one (1D: 24 bits of width; 3D: 11, 11 and 10 bits; 2D and cube:
  13 and 13 bits), each with 1 added, or 3 when +40 has bit 31; written to *r4, *r5, *r6 in that order, the word at
  +36 read again after each write. `any360::xdk::D3dTextureDimensions` (`d3d.h`), host kind
  `xdk::d3d_state::TextureDimensions`. When every address is below 0x7EFFFF00 and no size is written over the word
  at +36, the sizes are read once and written straight off the base.
- `sub_826AD108`, a texture level's bytes: r3-r5 its width, height and depth words aligned in place (the width to the
  format's block width x a pitch of 32 blocks, or 256 bytes of blocks when that is more without r9; the height to the
  block height, x32 unless r8 is 0; the depth to 4 for a volume, r8 = 2), then (width x r6 bits / 8) x height bytes
  rounded up to 4 KB, x depth, or the volume's whole size rounded. `any360::xdk::D3dTextureLevelBytes`, host kind
  `xdk::d3d_state::TextureLevelBytes`. Declined: a block of 0 bytes without r9 (the runtime's division by 0).

Their instruction-by-instruction references: `d3d_reference.h`, `reference::resource::TextureDimensions` and
`TextureLevelBytes`.

Two things the in-game self-test caught that the unit test (model against my reading of the code) could not:
- The level's bytes are a `mullw` into r3: on the 64-bit PowerPC the signed 64-bit product of the two words, not its
  low half. With the city's real sizes the product fits in 32 bits (verify: 0 different), with the self-test's random
  ones it does not (1515 of 2700 different at first). Model and reference now give the signed 64-bit product; the unit
  test checks more than 1000 calls past 32 bits.
- `std::max({...})` over the four addresses compiled to a call into MSVC's STL (`__std_max_4u`, the array spilled):
  the dimensions' native fell to 0.55-0.67x. The test is now their OR (each is at most it), and the native is at the
  original's time.

## Tests

- Unit test (`xdk_d3d_tests`, `TextureSizes`): the dimensions on 100,000 random fetch constants (every dimension,
  +40's bit 31; the outputs apart, the same word, or over the word at +36, whole or in part); the level's bytes on
  200,000 random calls (sizes small and any 32 bits, bits 0-128, formats 0-70 and past, dimensions 0-3, the fixed
  pitch on or off, the size words apart or the same); r3 and the memory against the references; 177,661 compared,
  22,339 declined (a block of 0 bytes without the fixed pitch: memory left as it was); 0 bytes and 0 allocations
  (`unit_test.txt`).
- In-game self-test (3 launches, `selftest_tex3_*.txt`), the title's code against the native on random inputs:

| function | calls | different | original (median) | native (median) | speed | memory a call (original / native) |
|---|---:|---:|---:|---:|---:|---|
| texture dimensions | 3 x 3000 | 0 | 7.5 ns | 7.5 ns | 1.00x | 0 B, 0 allocations / 0 B, 0 allocations |
| texture level bytes | 3 x 3000 (300 declined) | 0 | 38.8-42.5 ns | 12.5 ns | 3.10-3.39x | 0 B, 0 allocations / 0 B, 0 allocations |

  The dimensions are a dozen instructions: at the self-test's 1.25 ns resolution the two sides tie.
- Verify mode in the city (`--any360_xdk_verify=true`, the two counted together as "D3D texture size"): the version committed 219,463 calls in a
  150 s route, 0 different (`verify_tex_132.txt`); before the dimensions' last change 194,045, 0 different
  (`verify_tex_108.txt`).

## In game

Bound, both. 5 v 5 launches of 150 s on the Radeon (cores 8-15), alternating, after first, the same route
(`ab_tex_runs.txt`, `ab_tex_off_runs.txt` and their `*_lows.txt`, tools/frame_lows.py):

| | before (records on) | after (records on) | before (records off) | after (records off) |
|---|---:|---:|---:|---:|
| FPS (steady periods, mean / median) | 63.1 / 63.1 | 63.1 / 63.1 | 63.2 / 63.2 | 63.2 / 63.2 |
| 10% low | 62.3 | 62.3 | 62.7 | 62.6 |
| 1% low (mean / median) | 60.6 / 61.6 | 59.9 / 61.6 | 61.8 / 61.8 | 61.8 / 61.8 |
| private memory | 2586 MB | 2586 MB | 2584 MB | 2584 MB |

No difference: rank z between -0.31 and +1.04 with records on, between -1.67 and +0.94 with records off (the two
are about 200,000 calls a route). Another session's small Vulkan probe windows (cores 0-7, NVIDIA) overlapped the
first records-on pairs; they are in line with the others.

Earlier versions' self-test reports, kept: `selftest_tex_1.txt` (the level's bytes in 32 bits: 1515 of 2700
different) and `selftest_tex2_1.txt` (the 64-bit product fixed, the dimensions' check through `std::max`: 0.67x).
