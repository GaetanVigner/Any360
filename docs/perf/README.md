# Performance archive

Every step's measured results, kept for later optimization passes: the self-test report (time and allocations per call against the title's), its published table, and the in-game A/B runs with their frame-time analysis (`tools/frame_lows.py`). Each folder is one step, newest last. The A/B runs files list each launch's log (`=== <variant> (<log>)`) with its last frame-rate lines; the logs themselves stay in the title's build folder.

- [2026-11-14-d6-step1-direct-submit](archive/2026-11-14-d6-step1-direct-submit/): D6 step 1: command buffers submitted directly (published as of Any360 018eb50)
- [2026-11-14-d6-step2-segments](archive/2026-11-14-d6-step2-segments/): D6 step 2: the kick's segments native (68-case self-test report; segments on/off A/B) (published as of Any360 659be0c)
- [2026-11-14-d6-step3-tiled-recording](archive/2026-11-14-d6-step3-tiled-recording/): D6 step 3: tiled recording native, tile bin selects direct (published as of Any360 42712f8)
- [2026-11-14-d6-step4-begin-end-tiling](archive/2026-11-14-d6-step4-begin-end-tiling/): D6 step 4: BeginTiling / EndTiling native (published as of Any360 c02cff0)
- [2026-11-15-d6-step5-tile-ops](archive/2026-11-15-d6-step5-tile-ops/): D6 step 5: the DPC's tile ops interpreter native (published as of Any360 2057e5c)
- [2026-11-15-d6-step6-ring-out](archive/2026-11-15-d6-step6-ring-out/): D6 step 6: the ring out of Crackdown's path (published as of Any360 b832638)
- [2026-11-15-d6-frame-lows](archive/2026-11-15-d6-frame-lows/): D6 measured with median and 10% / 1% lows: 15 launches, three variants (frame_lows.txt) (published as of Any360 d57d69a)
- [2026-11-16-texture-loads](archive/2026-11-16-texture-loads/): texture loads of 1 and 2-byte texels, tiled and linear; the dip's slowest frames broken down (NOTES.md)
- [2026-11-16-d3d-alloc-per-call](archive/2026-11-16-d3d-alloc-per-call/): the 68-case self-test with host allocations a call on each side (BenchmarkDotNet style), and the published table
- [2026-11-16-hitches](archive/2026-11-16-hitches/): the title-side hitches: hitch tracer reports, the physical allocator's skip-ahead (5 v 5 A/B: 1% low 203 -> 221 FPS) (NOTES.md)
- [2026-11-16-zlib-inflate-fast](archive/2026-11-16-zlib-inflate-fast/): zlib 1.1.4's inflate_fast native: benchmark, the check and the measure in game (1.4x a call), 5 v 5 A/B (the dip's slowest frame 14.6 -> 9.4 ms) (NOTES.md)
- [2026-11-16-libpng-filter-row](archive/2026-11-16-libpng-filter-row/): libpng 1.0.5's png_read_filter_row native: benchmark (Paeth in SSE2, 3.4-5.0x), the check and the measure in game (10.1x a call), 5 v 5 A/B within noise (NOTES.md)
- [2026-11-17-xmemset](archive/2026-11-17-xmemset/): the XDK's vector fill (XMemSet) native: the city's CPU profile that chose it, benchmark, the check and the measure in game (1.7x a call over 10.5 M calls) (NOTES.md)
- [2026-11-17-d3d-refcounts](archive/2026-11-17-d3d-refcounts/): 2026-11-17, D3DResource_AddRef / D3DResource_Release native (D1)
- [2026-11-17-crt-sin-cos-asin](archive/2026-11-17-crt-sin-cos-asin/): the C runtime's sin, cos and asin native, bit for bit: benchmark, the check and the measure in game (1.05x, 1.2x, 1.3x a call over 19 M calls) (NOTES.md)
- [2026-11-17-d3d-render-states](archive/2026-11-17-d3d-render-states/): 2026-11-17, SetRenderState_ViewportEnable and SetRenderState_DepthBias native (D1)
- [2026-11-17-crt-atan2-pow](archive/2026-11-17-crt-atan2-pow/): the C runtime's atan2 and pow native, bit for bit: benchmark, the check and the measure in game (1.6x, 3.7x a call), and the host's C library beside the five math functions (1.4-6.6x faster, 23-47% of its results not the title's) (NOTES.md)
- [2026-11-17-d3d-surface-setters](archive/2026-11-17-d3d-surface-setters/): 2026-11-17, SetRenderTarget and SetDepthStencilSurface native (D1)
- [2026-11-18-d3d-render-state-fields](archive/2026-11-18-d3d-render-state-fields/): 2026-11-18, the render state fields, their getters and the sampler state siblings native (D1)
- [2026-11-18-sys-step12](archive/2026-11-18-sys-step12/): step 12's first 27 system calls (13 cryptography, 14 Win32 forms): time and host memory a call (0 B, 0 allocations) beside Windows' CNG and `wcslen`; SHA-256, HMAC-SHA-1 and DES slower than CNG (0.15-0.6x), MD5 at par (NOTES.md)
- [2026-11-18-xaudio-reverb](archive/2026-11-18-xaudio-reverb/): XAudio's I3DL2 reverb native, its own statements made into one leaf (tools/ppc_model.py): benchmark, the check and the measure in Crackdown's city (1.15x a frame, 0 different), the state found 1 KB longer (NOTES.md)
- [2026-11-18-d3d-viewport-chain](archive/2026-11-18-d3d-viewport-chain/): 2026-11-18, the viewport and scissor chain native (D1)
- [2026-11-18-sys-step12-opt](archive/2026-11-18-sys-step12-opt/): the same calls optimized: SHA-1 / SHA-256 on SHA-NI (2.8-5.0x CNG for SHA-1, 1.0-2.7x SHA-256), MD5 unrolled (1.0-1.7x), table DES (2.3-3.4x, triple DES at parity), lstrlenW on the host's wcslen; CNG behind a switch for processors without SHA-NI (NOTES.md)
- [2026-11-18-sys-step12-info](archive/2026-11-18-sys-step12-info/): step 12's third batch, xam.xex's 19 information calls: 1-21 ns a call, 0 B and 0 allocations; no host equivalent (NOTES.md)
