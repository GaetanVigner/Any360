# Step 12, the calls Xenia stubs too: the 5 module and loader calls

`XamLoaderGetMediaInfo`, `XamLoaderGetMediaInfoEx`, `XamUpdateGetBaseSystemVersion`,
`XamUpdateGetCurrentSystemVersion`, `XexLoadExecutable`, answered as Xenia's (xam_content.cc, xam_info.cc,
xboxkrnl_modules.cc), with the title's id where Xenia has none.

Measured on 2026-11-21 (AMD Ryzen 7 7800X3D, cores 0-7) with `sys_abi_bench 9`, three times
(`sys_abi_bench_run1-3.txt`); every row's result is checked after its timing. No host call does this work.

Reused, not recoded: `XexLoadImage`'s answer (the title's module, the system module, nothing else), the title's id,
and the kernel build the layer reports in `XboxKrnlVersion`.

| call | original ns | Any360 ns | speed | B, allocs a call |
|---|---|---|---|---|
| XamUpdateGetBaseSystemVersion, XamUpdateGetCurrentSystemVersion | 70-72 | 1.4-2.0 | 34-50x | 0, 0 |
| XamLoaderGetMediaInfo, XamLoaderGetMediaInfoEx | 71-88 | 3.3-6.9 | 10-27x | 0, 0 |
| XexLoadExecutable (a module not loaded) | 69 | 24.9-27.7 | 2.5-2.8x | 0, 0 |

`XexLoadExecutable` is `XexLoadImage`: the module name read, lowered and compared.

The answers:
- **`XexLoadExecutable`**: `XexLoadImage`'s answer, as in Xenia. The title's own module (default.xex or its name) and
  the system module (xam.xex, xboxkrnl.exe) give their handle; anything else answers STATUS_NO_SUCH_FILE with a
  handle of 0.
- **`XamLoaderGetMediaInfo(Ex)`**: the title runs from an Xbox 360 game disc (1). The title id is the running
  title's (Xenia gives 0, having none to give). The Ex form's third word is 0. There is no result.
- **System versions, as build numbers**:
  - base: the first retail kernel's, 1888, as Xenia;
  - current: the build the layer reports in `XboxKrnlVersion` (0xFFFF, newer than any title asks for), so a title
    checking for a system update finds none needed. Xenia gives 1888 for both.

Checked by `sys_adapter_tests` (`ModuleAdapters`):
- each adapter is called with its arguments in the registers a title uses, r3 and the guest bytes checked;
- `XexLoadExecutable` gives the same answer as `XexLoadImage` for the title's module, the system module and an
  unknown one;
- the media info's two and three words, with r3 untouched.
