# 2026-11-19: 94 render and sampler state fields native (D1)

Picked by a new count of the route's calls (`call_counts.txt`: the 453 functions of Crackdown's D3D range that no
binding covered, 270 of them called, 59 M calls in 150 s), then classified strictly from the recompiled code
(`classify_fields.py`): 99 functions of a dozen tiny shapes (2-19 instructions) that set or read one field of a
register the runtime keeps in its device, a stage's texture fetch constant word, or a state as the title set it.
94 bound; the 5 left out are called once a route or read a word whose state is not known for sure.

What they are, named from the register definitions (Mesa's a2xx.xml, the SDK's xenos.h texture fetch layout) and the
runtime's flush (which register each dirty bit sends):

- SetRenderState and GetRenderState of RB_DEPTHCONTROL's fields (ZFunc, the stencil functions and operations for
  both faces, TwoSidedStencilMode), PA_SU_SC_MODE_CNTL's (FillMode, MultiSampleAntiAlias, PrimitiveResetEnable),
  RB_COLORCONTROL's (AlphaToMaskEnable and offsets), RB_HIZCONTROL's bits, SQ_WRAPPING_0/1 (D3DRS_WRAP0-15, a nibble
  each: the runtime ORs the value's bits above the nibble in too), RB_STENCILREFMASK(_BF)'s bytes (stencil ref, mask,
  write mask for both faces), VGT_MULTI_PRIM_IB_RESET_INDX; getters of the states kept as set (ZEnable,
  StencilEnable, ScissorTestEnable, ColorWriteEnable0-3, HighPrecisionBlendEnable0-3, BlendOp, BlendOpAlpha).
- SetSamplerState and GetSamplerState of a stage's fetch constant (AddressU/V/W, MipFilter, TrilinearThreshold,
  BorderColor, the gradient exponent adjusts, force_bc_w_to_max), MaxAnisotropy and MaxMipLevel as kept.

One descriptor each (`any360::xdk::d3d_fields`, `D3dField` in `d3d_state.h`): the field of the device's layout, its
bits, the dirty bits; `D3dSetField` / `D3dGetField` run them, the host kind `xdk::d3d_state::Field<F>` binds them.
Four layout fields added (reset_index, wrapping, stencil_ref_mask, stencil_ref_mask_back).

## Tests

- Unit test (`xdk_d3d_state_tests`, `TestStateFields`): each descriptor against its function's own code: the shape
  and operands read off the recompiled function (rlwimi / rlwinm sh, mb, me with the wrapping masks, the dirty word
  and bits, lhz, lbz, cntlzw, subfic/subfe's 64-bit all-ones), run instruction by instruction by
  `reference::fields::Run`. 3000 calls each on random devices, values (small, any 64 bits) and stages (past 31 now and
  then): 279,041 compared, 2,959 declined (stage past 31, memory as it was), 0 failing; 2.44 ns a call against the
  reference's 5.76; 0 bytes and 0 allocations (`unit_test.txt`).
- In-game self-test (`Field_*` cases, 2 launches of 600 s, `selftest_fields_*.txt`): 94 cases, 0 different; the two
  sides' medians summed over both launches: original 653.4 ns, native 599.2 ns (1.09x); 41 faster, 47 equal, 6 one
  timer tick (1.25 ns) slower in one launch. Those 6 again in 2 launches (`selftest_fields6_*.txt`): all 1.00x or
  faster (equal ticks or 1.49x). They are 2-14 instructions: both sides are mostly the call.
- Verify mode in the city (`--any360_xdk_verify=true`, 150 s, `verify_fields_137.txt`): 25,371,278 calls of the 94
  verified against their own code, 0 different.

## In game

Bound, all 94. 5 v 5 launches of 150 s on the Radeon (cores 8-15), alternating, after first, the same route
(`ab_fields_runs.txt`, `ab_fields_off_runs.txt` and their `*_lows.txt`, tools/frame_lows.py):

| | before (records on) | after (records on) | before (records off) | after (records off) |
|---|---:|---:|---:|---:|
| FPS (steady periods, mean / median) | 63.2 / 63.1 | 63.2 / 63.1 | 63.2 / 63.1 | 63.1 / 63.1 |
| 10% low | 61.7 | 61.8 | 62.7 | 62.6 |
| 1% low (mean / median) | 58.6 / 61.6 | 60.6 / 61.7 | 61.8 / 61.8 | 61.8 / 61.8 |
| private memory | 2587 MB | 2584 MB | 2586 MB | 2584 MB |

No difference: rank z between -0.21 and +1.69 with records on, between -1.15 and +1.67 with records off (about
30 M calls a route, at a few tenths of a nanosecond each). The quiet window held (no other session's load).

Kept with the results: the route's call count of the range's unbound functions (`call_counts.txt`: address, calls in
150 s), the scripts that made it and the shapes (`gen_call_counter.py`, `classify_fields.py`) and the classified
operands (`fields.json`), for the next batches.
