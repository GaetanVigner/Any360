# 2026-11-16: the title-side hitches

What the slowest frames left after the texture-load work (2026-11-16-texture-loads) were made of, and the first fix.

## Tools added

- `any360_hitch_trace_ms=<ms>` (Any360 host): after 300 fast frames, a watcher thread samples every thread's stack
  every 2 ms while a frame runs longer than the threshold, and writes `hitch_<n>.txt` (at most 5 a run) to
  `any360_hitch_trace_folder` (default `logs`): threads by CPU cycles used, their stacks grouped by function.
- The report's slowest-frame line now also says what `any360::sys` did in that frame: file reads (count, MB, ms),
  waits that slept, XMA decoding time, physical allocations (count, ms).

## Findings

- `hitch_before_fix_2.txt` (a 215 ms frame): 29 of 46 samples in `MemoryManager::AllocRange` / the zeroing under
  `AllocatePhysical`. AllocRange tried every aligned window one page step at a time in a nearly full heap.
  Fixed: a window that holds a taken page (heap or physical) makes the search skip every window holding it
  (unit test: the fragmented heap in `sys_object_tests`).
- `hitch_before_fix_1.txt` / `_3.txt` (city frames of 20-120 ms): title code in 0x82A6E938-0x82A88750, the range
  `titles/crackdown/modules.json` left unnamed as "zlib, libpng and XeCrypt users", in two distinct call stacks:
  PNG decoding, confirmed below.
- Also seen: `NtQueryVirtualMemory` under sub_82380C78 (a free): probably the texture cache's write-watch faults,
  a renderer topic (unverified).
- Loading frames still show about 78 physical allocations taking 45 ms, mostly zeroing the pages; left as it is
  (skipping the zeroing is not safe while the physical space is shared).

## A/B: AllocRange skip-ahead (alloc_ab10.txt, alloc_frame_lows.txt)

5 launches a variant, alternating, same route, cores 0-7, NVIDIA GPU, steady periods without diagnostics:

| | before | after |
|---|---|---|
| FPS mean (median) | 300.8 (302.1) | 304.1 (299.8) |
| 10% low mean | 252.2 | 261.1 |
| 1% low mean | 203.2 | 220.8 (rank test z 2.06) |
| dip 1% low mean | 145.8 | 172.2 |
| dip slowest frame mean | 17.8 ms | 21.7 ms |
| dip frames over 16.7 ms mean | 0.4 | 0.8 |
| private memory mean | 2915 MB | 2898 MB |

Read with care: only the steady 1% low passes the rank test (z 2.06, over about 25 periods from 5 launches a side,
so marginal); the mean (z 0.55) and the dips (z 1.15) do not. The single slowest frame of a dip got worse: a frame of
about 25 ms in 4 of 5 launches after, against 2 of 5 before. Those are the title's own frames (above), which this fix
does not touch. The direct evidence is the hitch reports: the 215 ms loading frame spent 29 of 46 samples in
AllocRange and its zeroing before; after it, physical allocations take 0.1-0.7 ms in the city's slowest frames.
The old build hung once in the series (not counted).

## Confirmed: PNG decoding (libpng 1.0.5, zlib 1.1.4) on the title's main thread

The city stalls' two stacks, read against the decrypted image (the strings each function refers to, its callers and
callees in the recompiled code):

- 0x8270BA70 (the title's own loader, reached through a virtual call) creates a libpng read struct with version
  '1.0.5' (0x82A7A100, which passes '1.1.4' to inflateInit) and reads the image (0x82A7AA18 png_read_image,
  0x82A7A5A0 png_read_row).
- Stack 1 (6 of 25 samples, then 2 of 5): png_read_row -> 0x82A7EA10 inflate ('incorrect header check', 'need
  dictionary') -> 0x82A85390 inflate_blocks ('invalid block type', 'invalid bit length repeat') -> 0x82A878E8
  inflate_codes -> 0x82A88378 inflate_fast ('invalid distance code'): zlib 1.1.4's own call chain.
- Stack 2 (5 of 25, then 2 of 5): png_read_row -> 0x82A7F5D0 (png_read_filter_row: the row filters) and
  0x82A7C3F0 png_do_read_transformations ('NULL row buffer') -> 0x82A7B310 (a transformation).
- The NtQueryVirtualMemory samples (3 of 25) are under 0x82380C78, the title's free, which the same loader calls.

So half the main thread's samples in these stalls (15 of 30) are decoding PNG images, inflate and libpng's row work, and 19 of 30 with the loader's frees. The functions are named in
titles/crackdown/modules.json ("libpng 1.0.5 and zlib 1.1.4", 0x82A7A550-0x82A88750). 48 of its 194 functions have
an identical twin in The Simpsons Game, so a native replacement serves both titles. The image also holds two other
zlib copies (inflate 1.2.3 near 0x826A9C38, another near 0x8280E920), for later.

