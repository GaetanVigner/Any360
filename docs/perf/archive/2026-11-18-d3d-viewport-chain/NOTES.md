# The viewport and scissor chain native (D1), 2026-11-18

Crackdown's `sub_826AF370` (the window scissor's packets), `sub_826B1B60` (the scissor), `sub_826AF650` (the viewport on
its floats), `sub_826B1C60` (SetViewport) and `sub_826AF2D8` (a surface's viewport), on the AMD Radeon iGPU, cores 8-15.

- `selftest_vp_merged.txt` / `D3D_PERF.md`: the self-test (0 different, 1.05-1.64x, nothing allocated). The "Replay
  command buffer" lines are the earlier report's: in today's runs that case hung once (the binary without these
  bindings hung there too) and once gave 1 different call of 3000, at the device's cursor word. That suggests make room
  on the device copy (the buffer's +108 bit 5), a pre-existing problem of that case left for a follow-up.
- `verify_vp.txt`: the in-city verify lines (1.43 M, then 976 K calls of the chain, 0 different).
- `ab_vp_runs.txt` / `ab_vp_lows.txt`: the first 5 v 5, unusable: the other session's launches overlapped the after runs
  (about 110 s each) far more than the before runs.
- `ab_vp2_*`, `ab_vp3_*`, `ab_vp23_lows.txt`: 10 v 10 in quiet windows with the packets as records. The same steady
  63.1 FPS; 1% lows 60.9 against 59.5. Stretches of 19 ms frames 35-65 s into the city showed up in 9 of 10 after
  launches against 5 of 10 before launches.
- `ab_vp_pm4_*`: 5 v 5 with `--any360_d3d_records=false` (the native then writes the runtime's PM4 byte for byte): both
  63.2 FPS, 1% low 61.8, and no stretch in any launch. So records mode itself brings them (14 of 20 launches with
  records, none of 20 without).
- `ab_vp_final_*`: the batch as committed (the packets bound as PM4, records on as by default), 5 v 5 at 150 s, after
  first: 63.2 FPS both, 10% low 62.5 against 62.3, 1% low 60.0 against 58.1, private memory 2583 against 2585 MB.
