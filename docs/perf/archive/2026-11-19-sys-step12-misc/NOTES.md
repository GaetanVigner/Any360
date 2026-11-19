# Step 12, eighth batch: 9 calls (language, locale, memory, Xapi, IPTV, LDI)

The "misc" calls Xenia implements and the reference runtime stubs (docs/SYS_STUBS.md): `XTLGetLanguageV2`,
`XapipGetLocale`, `XamAllocEx`, `XapiFormatTimeOut`, `XapipCreateThread`, `XamIptvGetServiceName`,
`LDICreateDecompression`, `LDIDecompress`, `LDIDestroyDecompression`. Measured on 2026-11-19 (AMD Ryzen 7 7800X3D,
cores 0-7 while another session used 8-15) with `sys_abi_bench 9` (the original through the PowerPC ABI) and
`sys_step12_bench 9` (against the host), three times each: `sys_abi_bench_run1-3.txt`, `sys_step12_bench_run1-3.txt`.
The first measure, before the optimizations below, is in `before/` (all cores, the machine idle).

Reused, not recoded: the console's settings and XAM's online language and country tables (`info.cpp`), `XamAlloc` and
`MmAllocatePhysicalMemoryEx`'s allocator (`memory.cpp`), the Win32 timeout (`Win32Timeout`), `ExCreateThread` and its
processor mask (`scheduler.cpp`), the kernel heap that holds XAM's strings, the scheduler's dispatcher (the title's
LDI allocator runs on the calling thread as an APC does). Host libc has no LZX; Windows' cabinet.dll has one inside
its CAB API (FDI), which is the host column, and its encoder (FCI) is the tests' reference. Xenia decodes with
libmspack (LGPL): Any360's decoder is its own, written from the format.

Against the original (the reference's stub, 67-72 ns a call, doing none of the work; Any360 ns over three runs):

| call | original ns | Any360 ns | speed | B, allocs a call |
|---|---|---|---|---|
| XapiFormatTimeOut | 68-71 | 3.2 | 21-22x | 0, 0 |
| XamIptvGetServiceName | 69-70 | 4.0-4.2 | 16-17x | 0, 0 |
| XTLGetLanguageV2 | 69-72 | 4.4-4.7 | 15-17x | 0, 0 |
| XapipGetLocale | 68-69 | 14-16 | 4.3-4.9x | 0, 0 |
| LDICreateDecompression + Destroy (scratch) | 137-139 (two stubs) | 42-45 | 3.1-3.2x | 0, 0 once warm |
| XamAllocEx virtual 9 KB, + XamFree | 67-70 | 105 | 0.6-0.7x | 0, 0 |
| XamAllocEx physical 9 KB, + XamFree | 68-71 | 147-150 | 0.5x | 0, 0 |
| LDIDecompress, a 1 MiB stream (32 chunks) | 2265-2330 (32 stubs) | 2.61-2.69 ms | - | 0, 0 |

`XapipCreateThread` is left out of the ABI bench as `CreateThread` is: it is `ExCreateThread` (measured with the
kernel calls) plus its flags, 1.2 ns (`sys_step12_bench`). The rows under 1x do the work the stub skips (it returns
at once and leaves the title without memory or data): `XamAllocEx` finds and zeroes 12 KB of pages and the row also
returns them, `LDIDecompress` decodes 1 MiB (380-403 MB/s, below).

Against the host (`sys_step12_bench`, ns a call, the fastest of 9 runs, three runs):

| work | bytes | Any360 ns | host ns | speed |
|---|---|---|---|---|
| XamAllocEx physical + XamFree, against VirtualAlloc + VirtualFree | 9029 | 142.5-142.7 | 2492-2561 | 17.5-17.9x |
| XamAllocEx virtual + XamFree | 9029 | 101.7-102.4 | 2492-2561 | 24.4-25.2x |
| XamAllocEx physical + XamFree | 262144 | 1949-2004 | 2926-2970 | 1.47-1.52x |
| XamAllocEx virtual + XamFree | 262144 | 1845-1849 | 2926-2970 | 1.58-1.61x |
| LDI stream (Create, 32 chunks, Destroy) against FDI decoding the same CAB, text and code, 2^17 window | 1 MiB | 2.72-2.76 ms (380-386 MB/s) | 2.74-2.75 ms (381-382 MB/s) | 1.00-1.01x |
| the same, 2^21 window | 1 MiB | 2.60-2.62 ms (400-403 MB/s) | 2.70-2.71 ms (388-389 MB/s) | 1.03-1.04x |
| the same, random bytes (uncompressed blocks), 2^17 | 1 MiB | 0.38-0.40 ms (2651-2750 MB/s) | 0.97-0.98 ms (1067-1082 MB/s) | 2.48-2.55x |

The LDI rows' bytes (844 B and 11.8 KB a stream on average) are the first stream's decoder and window, kept and
reused by every later context: 0 allocations a stream once warm (`sys_ldi_tests` checks it). FDI allocates its own
state each CAB (1.2 KB counted through its allocator).

- **LZX match copy**: byte by byte first (`before/`, all cores, the machine idle): 386-417 MB/s, 1.03-1.10x FDI.
  Copying eight bytes at a time to the match's end and past it (up to seven bytes, into a margin) measured 1.09-1.24x
  (`rejected-overshooting-copy/`) and was rejected: once the window has wrapped, the bytes past a match are the
  previous lap's, which a later match at the far edge (offset window - 3) still reads. Kept: eight bytes at a time
  while they fit in the match, the rest byte by byte (1.00-1.04x FDI: most matches are short), and a test with
  far-edge matches after short ones on every window size.
- **XapipGetLocale**: 42-43 ns with `snprintf`, 13-16 ns with the two strings copied: kept.
- **XamAllocEx**: the allocator's own cost (a page search under the memory manager's lock and the zeroing); the host's
  VirtualAlloc does the same work through the kernel and is 1.5-25x slower.
- Not covered here: the title's LDI allocator and freer through the real dispatcher (the tests call a fake one; the
  adapter runs them as an APC runs), a Linux build, a title calling these in game.
The answers (Xenia's xam_locale.cc, xam_info.cc, xboxkrnl_ldi.cc and ldi_decompressor.cc):

- `XTLGetLanguageV2`: the console's language under Swedish (13), else the region's (English outside NTSC-J).
- `XapipGetLocale`: the legal locale ("en-US") as ANSI, truncated and terminated; E_NOTFOUND for an unknown language or
  country, ERROR_INSUFFICIENT_BUFFER past XAM's 7-character buffer.
- `XamAllocEx`: XamAlloc without bit 30 of the flags; else physical 4 KB pages, the protection from bits 28-29 and the
  alignment from bits 24-27 of the physical flags (4 KB when 0), zeroed; the address always written; E_OUTOFMEMORY.
- `XapiFormatTimeOut`: -10000 x ms written big-endian, its guest address returned (Xenia returns its host address); 0
  and nothing written for INFINITE.
- `XapipCreateThread`: the 7-argument form of CreateThread; the processor (0-7) is the kernel's affinity bit 24 + n
  (Xenia's shift overflows past 7: none is asked then).
- `XamIptvGetServiceName`: a 0x78-byte name laid out once in the kernel heap; empty (the console's IPTV setting; Xenia
  writes its own name there).
- LDI: Xenia's results (MDI_ERROR_*), the source minimum (data block max + 0x1800), windows 2^12-2^22 accepted and
  2^15-2^21 decoded, the context 0x2FF8 then the window + 0x105 from the title's allocator (the window and then the
  context freed, the freer outside the lock), the "LDIC" magic, each call one chunk into one frame of up to 32 KiB.
  An odd uncompressed block's padding byte is taken from the chunk that holds the block's end when it has one (where
  cabinet.dll's encoder writes it), else from the next chunk's start.

Checked by `sys_ldi_tests`: 64 streams of cabinet.dll's LZX encoder (text, random bytes, x86-like code with E8 calls,
far repeats, far-edge matches after short ones, 1 byte to 2.5 MB) on every window size from 2^15 to 2^21 decoded byte for byte (verbatim, aligned and uncompressed blocks all met: 582, 430 and 562 in the first 59 streams; 52 streams with the E8 translation), and again after a reset; hand-built
streams (odd uncompressed blocks, padding in either chunk, a block across chunks); corrupt chunks and 3000 random
ones refused; FDI's output equal to the data; the contexts' parameters, results, allocator and freer order, failure
path, magic, a decoder-less window, 0 host allocations once warm. `sys_object_tests` (`MiscStep12EighthTests`): the
languages 0-20, the locale at every length, another language and country, an unknown country, the timeouts, the
thread flags and a thread's processor, IPTV's buffer (0 allocations), XamAllocEx's seven flag cases (protection,
alignment, zeroed pages, the address range) and its failure.
