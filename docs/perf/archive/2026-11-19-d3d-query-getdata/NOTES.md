# 2026-11-19: IDirect3DQuery9::GetData native (D1), not bound yet

Crackdown's `sub_826C0D38`, the most called runtime entry Any360 did not cover (about 1.6 M calls in a 150 s city
route: the title polls its queries). Native in `any360/xdk/d3d_state.h` (`D3dQueryGetData`, and
`D3dQueryGetDataPlan` for the verify mode), its instruction-by-instruction reference in `d3d_reference.h`
(`state::QueryGetData`), the host kind `xdk::d3d_state::QueryGetData` (its kick: the title's make_room through
`g_make_room`, from GetData's 208-byte frame).

What it does: event queries (8), occlusion queries (9: the GPU's little-endian counters summed over the query's blocks,
read through the physical view at 0xC0000000) and extent queries (10: x / y in eighths, the depth as the runtime's
float-depth bits or a float of the 24-bit value); S_FALSE while the GPU is behind the query's frame, with a kick of the
command buffer when the query was issued this frame.

## Tests

- Unit test (`xdk_d3d_state_tests`, `TestQueryGetData`): the model against the reference on 20,000 random calls in
  memory laid out in the 0xC0000000 view (both physical forms of a block address, unwritten blocks, counts past 30,
  float depth on and off, the data over the query, the device or a block); every byte of the data, the result and the
  kicks; the plan agrees with the model on every call; the float-depth bits on all 65,537 inputs. 17,495 calls
  compared (3,426 event, 6,225 occlusion, 5,565 extent, 2,279 other types), 2,320 kicks, 2,505 declined, 0 failing
  (`unit_test.txt`).
- In-game self-test (`Query GetData`): the title's code and the native on the same random queries, blocks in physical
  memory the self-test now allocates (`CallSetup::physical`): 0 different, 2.00-2.08x, 0 B and 0 allocations a call
  on both sides (`selftest_*.txt`). Kicks are left out of the case: through Crackdown's hook on make_room
  (`D3dRingSegmentSwitch`) they would reach the real ring's segment-switch bookkeeping.
- Verify mode in the city (`--any360_xdk_verify=true`, bound): 1,671,643 and 1,647,997 calls verified, 0 different;
  3 calls differed at first and matched when run again (the GPU finishing a block between the two runs), the kicks
  declined (`verify_*.txt`).

Two self-test harness fixes came with it:
- The data never over the query in the case: the timed calls run 8 in a row on what the call before left, and the
  original's write of 1 into the query's device word made the next call read a device at address 1 (a crash).
- MXCSR: the recompiled code keeps the flush-to-zero bits cached in its context and sets MXCSR only when the cache
  differs. The self-test's contexts are copies of the one taken when it started, while its slices run inside later
  title calls whose MXCSR may have flush on: the original then flushed an extent query's denormal depth (3 different
  in one launch, byte 21 of the data). Each side now starts with the MXCSR its context claims, and each slice gives
  the title's thread its MXCSR back.

## In game: not bound

5 v 5 launches of 150 s on the Radeon (cores 8-15), alternating, after first, the same route:

| | before (records on) | after (records on) | before (records off) | after (records off) |
|---|---:|---:|---:|---:|
| FPS (steady periods) | 63.0 | 63.0 | 63.0 | 63.0 |
| 10% low | 62.3 | 62.1 | 62.6 | 62.5 |
| 1% low (mean / median) | 59.8 / 61.6 | 58.5 / 61.4 | 61.7 / 61.7 | 61.7 / 61.7 |
| private memory | 2594 MB | 2594 MB | 2585 MB | 2583 MB |

Records off: no difference (rank z between -1.2 and +1.2). Records on (the default): the 1% lows lower with the
native (z -2.33); a second 5 v 5 gave the same (z -1.98), 10 v 10 together: 1% low 58.4 against 60.3 (z -3.10),
10% low z -2.02. With records on every launch of both arms has the known stretches of 35 FPS frames (the records
mode's step of one more segment and two more fences a frame); the native moves more of them into the steady periods.
Likely why: the title polls its queries, and a query issued this frame and not done kicks the command buffer at
every call (make_room: a new segment and a fence); the faster GetData turns the poll loop faster and kicks more
often. Not bound until the records mode's fence flushes are fixed (the records session has the lead); then measured
again.

Files: `ab_query_runs.txt`, `ab_query_runs2.txt`, `ab_query_off_runs.txt` (the launches) and their `*_lows.txt`
(tools/frame_lows.py), `ab_query_lows_all.txt` (records on, 10 v 10).
