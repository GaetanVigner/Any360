# Glyph pages (docs/GLYPHS.md section 4, mechanism A by address), 2026-11-24

A title's pad glyphs redrawn from Any360's glyph sets, as texture patches: `any360/sys/png.h` (the atlases decoded),
`any360/gfx/texture_patches.h` (a guest texture shown with regions replaced in the host image, enlarged 4x) and
`any360/gfx/glyph_pages.h` (cells -> glyph query -> art fitted in each cell). First adapter: Mirror's Edge's
`UI_Fonts_XboxButtons` (its repository, `src/glyph_adapter.cpp`).

Benches (bench.txt; cores 0-7, RTX 5070 Ti machine):

| Call | Time a call | Bytes a call | Allocations a call | When |
| --- | --- | --- | --- | --- |
| `DecodePng` (xbox_one_128.png, 97 KB -> 1024x768) | 3.8 ms (834 MB/s out) | 3.2 MB (the inflated rows) | 2 | once per glyph set loaded |
| `ComposePatched` (256x256 page, 15 regions, 4x), before | 10.3 ms | 0 | 0 | |
| `ComposePatched`, after (each guest row enlarged across once, two bytes a lane) | 1.8 ms (5.8x) | 0 | 0 | per page, when its glyphs or content change |
| `BuildMips` (1024x1024, 10 levels) | 1.9 ms | 1.4 MB | 10 | per page, with the above |

Where the time goes in game (Mirror's Edge, 3 pages, 32 cells): nothing a frame while no glyph answer changes (a
generation read on the glyph thread). On a change of family or bindings: the glyph thread loads the set (two atlases,
~8 ms the first time) and fits the art (~ms), then each page is composed on a thread of its own (compose + mips, ~4 ms
a page): the renderer never waits, the page shows its new glyphs a frame or two later (gfx_render_tests: "shown 1
frames later"). Only a page never shown before waits for its first image (once, at the first prompt).

No original to compare against: the title draws its own page as it is (the patch replaces the upload of one 256x256
DXT5 page with a 1024x1024 RGBA8 image and its mips: 5.6 MB of GPU memory for Mirror's Edge's three pages). The host
has no PNG decoder of its own on every system (no zlib in the runtime or the libc), hence `DecodePng`; Python's zlib
made the test vectors (tools/make_png_vectors.py).
