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

| `tests/sys_object_tests.cpp` | Scenario tests of the object manager against the NT kernel's documented semantics: sequences of calls checked on statuses, returned states and guest fields, handles abstracted (no reference runtime: it is not an oracle here, see below) |

A system call is *native* on the progress page only when it is in `kExports` and one of the oracles proves it equal
to the reference, or, where the reference is not faithful, when the scenario tests prove the documented NT semantics.

## What is verified (128 system calls)

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
- **The object manager** (`objects.h`, scenario tests): the handle table (handles from 0xF8000000 by four, slots
  reused, duplicates, `DUPLICATE_CLOSE_SOURCE`, the current-thread pseudo handle, Ob references apart from handles),
  events (`KeInitializeEvent`, `KeSetEvent`, `KeResetEvent`, `KePulseEvent` returning the previous state, the Nt*
  forms with their previous-state outputs, `NtQueryEvent`, named events), semaphores (count and limit, the limit
  error), mutants (owner, recursion, `STATUS_MUTANT_NOT_OWNED`), `NtClose`, `NtDuplicateObject`,
  `ObReferenceObjectByHandle`, `ObDereferenceObject`, and the single-object waits (`KeWaitForSingleObject`,
  `NtWaitForSingleObjectEx`: a signalled object is consumed at once, a relative timeout polls, zero does not wait).
  Nt-created objects are allocated from the kernel heap a title gives the layer (`ObjectManager::SetKernelHeap`).
  Timers (`KeInitializeTimerEx`, `NtCreateTimer`, `NtSetTimerEx`, `NtCancelTimer`: relative and absolute due times,
  periods, notification and synchronization types; expiry is noticed when the objects are polled by a wait or
  `Poll`), the guest clock (`KeQuerySystemTime`, FILETIME from the host clock) and the multiple-object waits
  (`KeWaitForMultipleObjects`, `NtWaitForMultipleObjectsEx`: wait-any returns the index taken, wait-all consumes every
  object together or nothing). Timer APC routines are not delivered yet.
- **The scheduler** (`scheduler.h`, scenario tests): guest threads as host threads, each with its KTHREAD, PCR, TLS
  block and stack in guest memory (the structures the rest of the layer reads through r13), run through the
  `Dispatcher` the title installs (the layer never executes guest code itself). `ExCreateThread` (stack size, thread
  id, XAPI start-up path, `CREATE_SUSPENDED`, the handle-is-object flag), `ExTerminateThread` (unwinds the host thread),
  `KeDelayExecutionThread` (relative and absolute), `NtYieldExecution`, `NtResumeThread` / `KeResumeThread` /
  `NtSuspendThread` (previous counts, `STATUS_THREAD_IS_TERMINATING`), `KeSetAffinityThread` (previous affinity through
  the pointer, as the Xbox kernel does), `KeQueryBasePriorityThread`, `KeSetBasePriorityThread`,
  `KeSetDisableBoostThread`. A thread is a dispatcher object signalled when it ends. Suspension is cooperative: a thread
  created suspended waits before its first instruction, a running one pauses at its next delay, yield or wait. The
  title process (KPROCESS) is created with the first thread when none is set.
- **The memory manager** (`memory.h`, scenario tests): the guest heaps as the kernel lays them out (virtual 4 KB pages
  below 0x40000000 and 64 KB pages above, the physical views at 0xA0000000 / 0xC0000000 / 0xE0000000 over one 512 MB
  physical space, page states free / reserved / committed with their protections and regions).
  `NtAllocateVirtualMemory` (reserve, commit, fixed addresses, large pages, top-down, sizes rounded as the kernel does,
  committed pages zeroed), `NtFreeVirtualMemory` (release of a region, decommit), `NtQueryVirtualMemory` and the region
  queries, `MmAllocatePhysicalMemoryEx` / `MmAllocatePhysicalMemory` (page size from the protection flags, alignment,
  address range, the views never overlap in physical space), `MmFreePhysicalMemory`, `MmGetPhysicalAddress`,
  `MmQueryAddressProtect`, `MmSetAddressProtect`, `MmQueryAllocationSize`, `MmQueryStatistics`, `XamAlloc` / `XamFree`.
  Addresses are the manager's own: the tests check the properties of regions, not the reference's numbers.
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

1. **Object manager and scheduler, continued**: absolute wait timeouts, APCs (timer and `NtQueueApcThread`),
   pre-emptive suspension, freeing kernel objects, `KeGetCurrentThread`-style queries.
2. **Memory, continued**: `NtProtectVirtualMemory`, `MEM_RESET`, the executable's image range as a queryable heap,
   `ExAllocatePool*`, `MmCreateKernelStack`, `KeLockL2` / `KeUnlockL2`.
3. **Files and XAM**: a VFS over installed data, content packages, user profile and input, then the Live-offline
   answers.
