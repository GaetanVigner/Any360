# 2026-11-28: the D3D device layout per title; Mirror's Edge binds ViewportEnable and DrawVerticesUP

## The change

- `xdk::g_d3d_layout` (xdk_natives.h) is no longer a constant fixed to Crackdown's runtime: a title sets its own with
  `SetD3dLayout` before its first D3D call. The default stays `D3dLayout{}`, Crackdown's values, which no title has to
  set.
- D3dLayout gains `begin_end` (13076: DrawVerticesUp's end of the vertices, the old `kBeginEnd`), `segment` (13140) and
  `segment_top` (14540) (the allocator's segment). The self-test's Begin and DrawIndexed setups read
  `segment`, `segment_top`, `shader_flags`, `gpu_done_frame`, `tiling_flags`, `tiling_pending[1]` and
  `vertex_declaration` instead of their Crackdown constants: the same values for Crackdown.
- `tools/d3d_layout_map.py <title>`: a title's layout read from its own D3D runtime. Every function of the title's D3D
  range is paired with Crackdown's whose mnemonics align best (both ways, ratio 0.85 or more); in the aligned
  instructions each displacement and each lis-built image address that differs gives a pair (Crackdown value, title
  value). A field takes the value its pairings give (two at least and 60% of them, or one that agrees with its
  segment); between two offsets past the first KB that move by the same amount, a field takes that shift
  (`segment`); anything else stays unknown. `mirrorsedge_layout_map.txt`: 155 functions paired, 81 of 101 fields read.
- `tools/d3d_layout_header.py`: the title's `src/d3d_layout.h` (unknown fields 0xFFFFFFFF) and the tests' data
  (`layer/xdk/tests/d3d_title_layouts.inc`): xdk_d3d_state_tests runs its layout cases on every title's layout and
  checks the setters' fields lie in the device.

Mirror's Edge's device: the registers' mirror (Crackdown 10240-10824) +128 bytes, the state as set (11552-12900) +280,
12944 +288, 13076 +368, 13140-16352 +376, the first KB unchanged.

## Mirror's Edge

| Function | Address | D3D self-test (3,000 calls) | Own code -> native | In game |
|---|---|---|---|---|
| SetRenderState_ViewportEnable | 82801570 | 0 different | 12.5 -> 10.0 ns (1.25x) | 1 call on route2, 0 different |
| DrawVerticesUP | 82804DD0 | run 1: 1 different (physical view, call 2471); rerun: 0 different | 348.8 -> 196.3 ns (1.78x); rerun 1.72x | declines the check by design (BeginVertices makes room) |

DrawVerticesUP's BeginVertices is the title's sub_82804938 and its copy sub_829E37C0 (memcpy). The first run's
difference sits in the physical view the cases share, as on 2026-11-27 (format block, shader link): not reproduced on
the same fixed-seed inputs. The other five D3D cases: 0 different (`d3d_selftest-run1.txt`). The bound C runtime and
D3D functions: 9.0 M calls in game, 0 different (`verify.txt`).

## Call of Duty: Black Ops (not bound: refresh-codbo's title)

`codbo_layout_map.txt` (its D3D range 0x824A0000-0x824C0000, no modules.json yet): 89 functions paired, 69 fields.
- GetType (824A4518) and the format block (824A0378): Crackdown's instructions exactly; bindable before this change.
- AddRef (824A4810) and Release (824A5638): the same instructions, other registers (a compare on r10 for r31, r3 for
  r31): to check with the D3D self-test; Release needs CoD's destroy function.
- ViewportEnable (824A8E48): the same instructions, its fields +128 as in Mirror's Edge (clip_control, vte_control
  read): bindable now with CoD's layout.
- DrawVerticesUP and LinkShaders: no function with Crackdown's instructions; CoD's begin_end is not read (its
  segment_top is +588).
