# 2026-11-18: the Replay command buffer self-test case made hermetic

The D3D self-test's "Replay command buffer (PM4)" case (sub_826C3750 against `d3d_draw::ReplayCommandBuffer`) hung
now and then on the AMD build, and gave "1 different" in about one launch in ten (always call 9, the device's cursor at
+48). Both came from the case's inputs, not from the native: the case runs the title's own helpers on a copy of the
device whose pointers are the title's.

## The hang: make_room on the device copy

A buffer that only makes room (+108 bit 5, one call in sixteen) has both sides call the title's make_room
(sub_826BB408). On the copy, make_room ended the scratch's segment with the copy's pointers, which are the title's:

- with no buffer being recorded (+12944 0) and no pending segments (+10872 0): sub_826BB1A0 -> sub_826BB348 ->
  sub_826BAD58, the scratch's segment (random words, its start the title's ring at +14552) kicked into the title's real
  ring. The GPU read those words and the title stopped calling D3D, so the self-test (it runs in slices at those calls)
  never went on: `hang_run1_room_only_calls.txt` (call 19 room only, the last line call 21), `hang_run1_thread_stacks.txt`
  (no thread running, the D3D thread waiting on an object). Whether the GPU stops depends on what it reads: run 3 in
  `hang_runs.txt`, the same state, passed. (The report printed under "--- run 1" there is an older report left in the
  folder; run 1 was stopped at 150 s.)
- with a buffer being recorded (+12944 set, run 2): the segment written into the recording's real list (+13000).
- make_room then waits for the frame before last (sub_826BB4F0) on the copy's fence, which never moves (a scratch word):
  the gate (a global, 0 in these runs) kept it off, but nothing in the case did.

Fix: the copy says its segment allocation failed and its frame's wait is done (+10813 bits 6 and 2): sub_826BA938
returns at once, sub_826BB1A0 only rounds the cursor, the wait is skipped. Only make_room and its callees read those
bits on this path (the original reads +10812 only, the shader helper +10813 bit 0). `room_fix_runs.txt`: 6 launches,
no hang.

## The 1 different: the bound shaders' link, in the title's memory

When the device was copied with shaders bound (+12416 vertex, +12412 pixel; most copies have none), the title's shader
helper (sub_826C6C48 -> sub_826C6B00) linked them in the vertex shader's own object (slot 0: +40 the pixel shader's id,
+64 the frame), the title's memory, not put back between the two sides. The original made the link, the native found
it made and took the other path: 265 words against 264. `call9_run8_different.txt` (vs 410EFA48, slot 0 +40
FFFFFFFF -> 7 after the original) against `call9_run7_same.txt` (no shaders bound), from `call9_runs.txt`.

Fix: no shaders bound on the copy in this case. Both sides call the same title helper for the shaders (the native's
FlushDirtyState calls rt.flush_shaders), so the native loses no coverage; the shader path stays covered where it is
the native's.

## After

`final_runs.txt`: 10 launches of the two Replay cases, no hang, 0 different in each (3000 calls each, 0 declined;
median 1.09-1.12x, 0 B and 0 allocations a call on both sides). `full_selftest.txt`: the whole self-test (101 cases) in one launch on the Radeon, to its end: 0 different in every case.
