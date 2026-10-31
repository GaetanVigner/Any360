# any360::sys: the native system layer

`layer/sys` replaces the system calls a title imports from `xboxkrnl.exe` and `xam.xex`. It is stage 1 of
[DESIGN.md](DESIGN.md): the reference runtime (ReXGlue) is the oracle each function is verified against, never the
implementation a title keeps.

## Layout

| | |
|---|---|
| `include/any360/sys/*.h`, `src/*.cpp` | `any360_sys`: portable C++23, no dependency. Every function takes guest addresses and a `Guest{base}` (a flat big-endian address space behind a host pointer), so the same code serves a static recompilation, a decompiled title and the tests |
| `ppc/exports.cpp` | `any360_sys_ppc`: one `any360_<Export>(PPCContext&, uint8_t*)` per system call, the kernel import thunk's calling convention (r3..r10, then the caller's stack slots at r1+0x54; result in r3); `kExports` lists them for a title's binder and for `tools/build_db.py`, which counts every `ANY360_SYS_EXPORT` as native |
| `tests/sys_io_tests.cpp` | Oracle 1: stateless calls. Links `rexruntimerd` from the reference SDK install, runs `__imp__<Export>` and `any360_<Export>` on the same random registers and two 16 KB guest blocks (strings, structures, va_lists, random printf formats with matching arguments), compares r3 and both blocks byte for byte |
| `tests/sys_runtime_tests.cpp` | Oracle 2: calls bound to the running thread. Sets the reference runtime up in tool mode (memory and kernel state, no GPU or audio), registers a host entry in its function table and creates a guest thread that runs the cases, so the reference implementations find their current thread, PCR and process. Native and reference run from the same state (blocks, PCR, KTHREAD, KPROCESS snapshotted and restored in between); r3 and every region are compared |
| `test.ps1` | Builds with the titles' toolchain (Visual Studio's clang++) against the SDK install named by `reference_sdk` in a `titles/<title>/pipeline.json`, runs both oracles |

A system call is *native* on the progress page only when it is in `kExports` and one of the oracles proves it equal
to the reference.

## What is verified (76 system calls)

- **Rtl**: memory (`RtlCompareMemory`, `RtlCompareMemoryUlong`, `RtlFillMemoryUlong`), strings (`RtlUpperChar`,
  `RtlLowerChar`, `RtlCompareString`, `RtlCompareStringN`, `RtlInitAnsiString`, `RtlInitUnicodeString`,
  `RtlMultiByteToUnicodeN`, `RtlUnicodeToMultiByteN`), time (`RtlTimeToTimeFields`, `RtlTimeFieldsToTime`),
  `RtlComputeCrc32`, `RtlNtStatusToDosError` (tables derived from Xenia, see NOTICE.md), critical sections
  (initialise, enter, try, leave), the no-op exception helpers.
- **CRT formatting**: `sprintf`, `_snprintf`, `swprintf`, `_snwprintf`, `vsprintf`, `_vsnprintf`, `vswprintf`,
  `_vsnwprintf`, `_vscwprintf`, `DbgPrint`: the Xbox CRT's printf dialect for strings and arguments in guest memory.
- **Ke / Kf on the thread's kernel structures**: IRQL, spinlocks (owner = PCR address), critical regions (APC disable
  count), TLS (slot bitmap in the KPROCESS, values in the KTHREAD's block), the process type,
  `KeQueryPerformanceFrequency` (50 MHz).
- **Configuration and information**: `ExGetXConfigSetting`, `XGetAVPack`, `XGetGameRegion`, `XGetLanguage`,
  `XamGetSystemVersion`, `XGetVideoMode` and the `Vd*` display queries (`VdQueryVideoMode`, `VdQueryVideoFlags`,
  `VdGetCurrentDisplayInformation`, `VdGetCurrentDisplayGamma`, `VdGetSystemCommandBuffer`,
  `VdInitializeScalerCommandBuffer`, the constant-return and no-op ones).
- **Offline networking**: `NetDll_inet_addr` (the Winsock parser), `NetDll___WSAFDIsSet`, `NetDll_XNetInAddrToString`,
  the XNet address conversions.

The display and console settings are `VideoSettings` / `ConsoleSettings` (defaults: 1280x720 at 60 Hz, English,
country 103); `SetDebugPrintSink` receives `DbgPrint` text.

## What the oracles taught

- The reference formats a negative 64-bit value with an unsigned conversion through a signed remainder (garbage digits),
  which no CRT does: the generator keeps unsigned 64-bit conversions non-negative.
- `ExGetXConfigSetting` writes `required_size` (0) even when it fails; `%a` ignores the precision; a critical
  section's lock count is kept in host byte order by the reference's atomics, so the layer does the same.
- `KeSetCurrentProcessType` sets the type on the current thread's KPROCESS (and on the PCR during a DPC).

## Where the reference stops being an oracle

Dispatcher objects (events, semaphores, mutants, timers) and handles are host objects in the reference: the guest
`signal_state` is not maintained after initialisation, `KeSetEvent` / `KeResetEvent` / `KePulseEvent` return 1 rather
than the previous state, and the first use stashes a host handle in the header's wait list words. Memory managers,
files and XAM state are host state too. A byte-for-byte comparison cannot verify a native implementation of these,
and the NT semantics (previous state returned, signal state in the object) are the ones a decompiled title relies on.

Next for the layer, in this order:

1. **Object manager and dispatcher objects**: the layer's own handle table and the NT semantics of events,
   semaphores, mutants and timers in the guest structures, verified by scenario tests (sequences of calls compared on
   statuses, returned states and the guest fields, with handle values abstracted) against the reference where it is
   faithful (statuses, `NtClose` of an unknown handle, named objects) and against the documented semantics elsewhere.
2. **Waits and the scheduler**: `KeWaitForSingleObject` / `NtWaitForSingleObjectEx` / multiple-object waits, delays,
   APCs, thread creation and the PCR / KTHREAD protocol the layer already follows.
3. **Memory**: the virtual and physical allocators (`NtAllocateVirtualMemory`, `MmAllocatePhysicalMemoryEx`, queries
   and protection), compared on the observable properties of regions rather than on addresses.
4. **Files and XAM**: a VFS over installed data, content packages, user profile and input, then the Live-offline
   answers.
