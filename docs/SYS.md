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

## What is verified (390 system calls)

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
  `NtWaitForSingleObjectEx`: a signalled object is consumed at once, a relative timeout polls, an absolute one (a system time) polls until
  then, zero or a time already past does not wait; the reference treats an absolute
  timeout as zero and does not wait).
  Nt-created objects are allocated from the kernel heap a title gives the layer (`ObjectManager::SetKernelHeap`).
  Timers (`KeInitializeTimerEx`, `NtCreateTimer`, `NtSetTimerEx`, `NtCancelTimer`: relative and absolute due times,
  periods, notification and synchronization types; expiry is noticed when the objects are polled by a wait or
  `Poll`), the guest clock (`KeQuerySystemTime`, FILETIME from the host clock) and the multiple-object waits
  (`KeWaitForMultipleObjects`, `NtWaitForMultipleObjectsEx`: wait-any returns the index taken, wait-all consumes every
  object together or nothing). A timer's APC routine is queued on the thread that set it at each expiry (see the DPC
  and APC delivery below).
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
- **The file system** (`files.h`, scenario tests on a temporary directory): guest devices mounted on host directories
  (read-only or writable), the kernel's symbolic links (`ObCreateSymbolicLink`, `ObDeleteSymbolicLink`, `game:`,
  `\??\D:`), case-insensitive resolution component by component (the same data works on case-sensitive hosts), never
  above a device's root. `NtCreateFile` / `NtOpenFile` (all dispositions, directory and non-directory options, delete on
  close, paths relative to a directory handle, the action in the I/O status block), `NtReadFile` / `NtWriteFile`
  (explicit offset or the file position, end of file, the completion event), `NtQueryInformationFile` and
  `NtSetInformationFile` (basic, standard, internal, position, mode, alignment, network open; rename, disposition,
  allocation, end of file), `NtQueryFullAttributesFile`, `NtQueryDirectoryFile` (one entry per call, wildcards,
  restart, entries sorted so the order is the same on every host), `NtQueryVolumeInformationFile`,
  `NtFlushBuffersFile`. Open files are objects of the object manager: `NtClose` of the last handle frees them (and
  deletes a file marked for deletion). The statuses are the NT ones (`OBJECT_NAME_NOT_FOUND` for a missing name,
  `OBJECT_PATH_NOT_FOUND` for a missing directory or device), which `RtlNtStatusToDosError` maps to the Win32 codes
  XAPI expects. Reads and writes complete at once (no asynchronous I/O yet); their completion APC is queued as the reference does.
- **XAM and the process** (`xam.h`, scenario tests): the answers of a retail console with one local profile signed
  in and no Xbox Live. Messages to the system applications (`XMsgStartIORequest(Ex)`, `XMsgInProcessCall`,
  `XMsgCancelIORequest`) complete at once; notifications (`XamNotifyCreateListener`, `XNotifyGetNext`: a new system
  listener first hears the sign-in, the guide opens and closes for `XamShowSigninUI`); the user (`XamUserGetSigninState`,
  `XamUserGetXUID`, `XamUserGetName`, `XamUserGetSigninInfo`, `XamUserCheckPrivilege`, profile settings read and written
  with the kernel's size protocol); sessions; message boxes and the device selector; enumerators; content packages
  (`XamContentCreateEx`, `XamContentClose`, `XamContentDelete`, `XamContentCreateEnumerator`, `XamContentGetDeviceState`:
  folders under the title's content root, mounted as `<root>:` on the file system while open); controllers
  (`XamInputGetCapabilities`, `XamInputGetState`, `XamInputSetState`, `XamInputGetKeystrokeEx`, through an input source
  the host provides; controller 0 by default); the title module (`RtlImageXexHeaderField`, `XexGetModuleHandle`,
  `XexGetProcedureAddress` through a host resolver, `XexCheckExecutablePrivilege`, `XamGetExecutionId`,
  `ExRegisterTitleTerminateNotification`); stops through a host hook (`KeBugCheck(Ex)`, `RtlRaiseException` except thread
  names, `HalReturnToFirmware`, `XamLoaderTerminateTitle`, `XamLoaderLaunchTitle`); the no-op process calls
  (`DbgBreakPoint`, `KeEnableFpuExceptions`, `KeLockL2`, `KeUnlockL2`, `KiApcNormalRoutineNop`); and the network of a
  console without a connection (`NetDll_WSAStartup`, `NetDll_XNetStartup`, `NetDll_XNetGetTitleXnAddr` with no address,
  sockets that fail with `WSAENETDOWN`, `NetDll_WSAGetLastError`).
- **The rest of Sonic '06** (MVP reached: all 178 of its system calls outside graphics and audio): `XeCryptSha` (equal to
  the reference), `XexGetModuleSection` (the title's resource sections), the kernel pool (`ExAllocatePool*`,
  `ExFreePool`: whole pages), `RtlUnicodeStringToAnsiString` / `RtlFreeAnsiString` (NT semantics: the length is set and
  characters above 0xFF become `?`; the reference converts to UTF-8 and leaves the length, which is why it is not the
  oracle here), `RtlUpcaseUnicodeChar`, `NtReadFileScatter`, launch data, device data, content creator, the file system
  cache size, inactivity, a title driver's device object and the `Io*` share-access calls, `ObIsTitleObject`,
  `ObReferenceObject`, the STFS device calls, and the console-key operations answered as a machine without its keys
  (`XeCryptBnQwBeSigVerify` accepts, `XeKeysGetKey` refuses, `XeCryptRotSumSha` zeroes its digest: no known title
  depends on that value, and Sonic '06's own port runs with it stubbed).
- **The rest of Crackdown** (MVP reached: all 248 of its system calls; scenario tests), answered as a retail
  console with one local profile and no Xbox Live:
  - the offline network: Winsock events (`NetDll_WSACreateEvent`, `WSACloseEvent`, `WSAWaitForMultipleEvents` with
    NT wait semantics: millisecond timeouts, the index of the event taken, `WSA_WAIT_TIMEOUT`; the reference returns 0
    whatever happened and reads the timeout in 100 ns units, so it is not the oracle there), the socket calls
    failing like the others (`getsockname`, `getpeername`, `WSAEventSelect`), XNet random bytes and system link keys
    (local: they work), key registration, and connections, server lookups and QoS failing with `WSAENETDOWN`, a lost
    connection status, `XNetQosListen`'s `ERROR_FUNCTION_FAILED` as the reference; `XNetLogonGetTitleID` from the
    XEX execution info;
  - XAM: tasks on a new guest thread (`XamTaskSchedule`), no voice device, no Live friends, an empty achievement
    enumerator, a failed stats enumerator (leaderboards are Live's), the gamer picture as placeholder ids and a plain
    image, the Live guide screens opening and closing at once, the online country from the console settings;
  - the kernel: `XexLoadImage` / `XexUnloadImage` for the title's module and the system modules (by name, a path's
    directories ignored; `XexGetModuleHandle` now answers the system modules too, as a console and the reference do),
    no other image; `NtDeviceIoControlFile` answering the cache partition's disk queries as the reference;
    `IoDismountVolumeByFileHandle`; the console-key signature calls passing as the reference; the floating-point
    state save and restore (the thread's own state, nothing to do); `XamContentLaunchImage` finding no image.
- **The rest of Test Drive Unlimited** (MVP reached: all 278 of its system calls; scenario tests):
  - the network: `XNetCleanup`, title server addresses not resolved (`WSAENETDOWN`), nothing to unregister
    (`WSAEINVAL`);
  - XAM: `XMsgCompleteIORequest` (result, extended error and length, the event set) and `XamGetOverlappedResult`
    (`ERROR_IO_INCOMPLETE` for a pending request unless asked to wait on its event), the virtual keyboard accepted at
    once with its default text (the reference's headless answer), a package's thumbnail kept next to its folder, the
    license mask (0, as the reference), the gamer tile write accepted, the notification position and delay, the DVD
    tray closed with the disc;
  - the kernel: DPCs (`KeInitializeDpc` lays the KDPC out, `KeInsertQueueDpc` / `KeRemoveQueueDpc` queue it once; queued
    DPCs run, which the reference never does: see the DPC and APC delivery below), `ObOpenObjectByName` / `ObReferenceObjectByName` on named
    objects (the name an `ANSI_STRING`, which the reference reads as a C string), `NtWriteFileGather` (pages listed
    one pointer each, as `NtReadFileScatter`), `IoSynchronousDeviceIoControlRequest` (the cache partition's disk
    queries; the reference accepts any code and writes nothing), `IoDismountVolume` / `ByName`, `MmMapIoSpace`
    (device registers are plain memory), `XexLoadImageHeaders` (the XEX2 headers of an image on a mounted device; the
    reference does not implement it), `VdSetDisplayModeOverride` and `XAudioGetSpeakerConfig` as the reference.
- **The rest of The Simpsons Game outside graphics and audio** (MVP reached: all 300 of those; scenario tests):
  - the network: the overlapped Winsock calls (`WSARecv`, `WSARecvFrom`, `WSASend`, `WSASendTo`,
    `WSAGetOverlappedResult`, `WSACancelOverlappedIO`) failing on sockets that never exist (`WSAENOTSOCK`), as the
    other socket calls; `WSASetLastError`, `WSASetEvent` / `WSAResetEvent` on the Winsock events; DNS and QoS service
    lookups completing at once with no answer (records from the kernel pool, their event set); `XNetGetOpt` giving
    back the startup parameters `XNetStartup` was given; no link, no debug address, no server or peer known, no
    newer title version; `XNetLogonGetMachineID` a fixed console id (`TitleSettings::machine_id`);
  - XAM: `XMsgSystemProcessCall` as the in-process form; the content device enumerator (the hard drive) and device
    names; a package's thumbnail read back; `XamContentResolve` giving a device path mounted on the package folder
    (`\Device\Content\<type>\<file name>`; the reference answers not found); `XamContentFlush`; no package to
    install; the Live guide screens (messages, players, achievements, invites...) opening and closing at once;
  - the kernel: `NtProtectVirtualMemory` (whole committed pages, never execute rights, as the reference),
    `NtSignalAndWaitForSingleObjectEx` (an event set, a semaphore or mutant released, then the wait),
    `ObOpenObjectByPointer`, `ObLookupThreadByThreadId` (the scheduler's threads), `NtQueueApcThread` (the APC queued
    on its thread and run at its next alertable wait, see below), `XexUnloadImageAndExitThread`.
- **The kernel side of The Simpsons Game's audio and video** (all 317 of its system calls native; scenario tests):
  the XMA context calls reading and writing the fields of the 64-byte context as the hardware packs them (big-endian
  words, bitfields from the low bit): `XMAInitializeContext` from its 56-byte init block, the input and output buffers
  as physical addresses (`0x80000000` for a buffer outside physical memory, as the reference), buffer validity, read
  and write offsets, loop data, packet metadata; `XMAEnableContext` / `XMADisableContext` handed to the `XmaBackend`
  (new optional `ContextEnabled` / `ContextDisabled`); `XMABlockWhileInUse`. `XMASetLoopData` reads an
  `XMA_LOOP_DATA` (the reference reads it as a whole context). `VdQuerySystemCommandBuffer`,
  `VdSetSystemCommandBuffer` and `XAudioQueryDriverPerformance` answer success and write nothing, as the reference.
- **Graphics and audio, the kernel side** (`graphics.h`, `audio.h`; scenario tests with a fake GPU, sink and XMA
  decoder): `VdInitializeEngines`, `VdInitializeRingBuffer` and `VdEnableRingBufferRPtrWriteBack` (handed to a
  `GpuBackend`, the command processor), `VdSwap` (the front buffer's fetch constant, made physical, and `XE_SWAP` written into the ring as the reference does), `VdSetGraphicsInterruptCallback` (the title's handler runs on a guest context
  of its own at dispatch level, for vertical blanks from a host clock and command-processor interrupts the backend
  raises), `VdPersistDisplay`; `XAudioRegisterRenderDriverClient` / `Unregister` (driver `0x4155000n`, the client's
  mixer callback pumped every 256 samples at 48 kHz), `XAudioSubmitRenderDriverFrame` (6 big-endian channels of 256
  floats, handed to an `AudioSink` interleaved by sample), the voice category volumes (unchanged, 1.0),
  `XMACreateContext` / `XMAReleaseContext` (64-byte contexts in physical memory, announced to an `XmaBackend`). Guest
  callbacks run through `Scheduler::CreateContext` and `Scheduler::Dispatch`. The renderer, the audio output and the
  XMA decoder are the backends still to write.
- **DPC and APC delivery** (scenario tests): the dispatcher a title binds takes up to 8 arguments (r3 to r10).
  - User APCs (`NtQueueApcThread`) run on their thread as routine(context, arg1, arg2) when it waits alertably
    (`KeDelayExecutionThread`, `KeWaitForSingleObject`, `NtWaitForSingleObjectEx`, `KeWaitForMultipleObjects`,
    `NtWaitForMultipleObjectsEx`, `NtSignalAndWaitForSingleObjectEx`), below the caller's stack. As in the reference,
    an alertable wait with APCs queued runs them and returns `STATUS_USER_APC` (0xC0) without waiting, and one that an
    APC queued from another thread interrupts runs it and returns the same.
  - DPCs run in order on a guest context of their own (a host thread started with the first one) as routine(dpc,
    context, arg1, arg2), at dispatch level with the PRCB's DPC-active flag set; a DPC leaves the queue before it runs,
    so it may queue itself again. The reference never runs them. On the console a DPC interrupts the processor that
    queued it; here it runs beside that thread.
  - Timer APCs (`NtSetTimerEx` with a routine): each expiry queues routine(context, system time low, high) on the
    thread that set the timer, as the reference; an alertable delay polls the timers, so one can end it.
  - I/O completion APCs (`NtReadFile`, `NtWriteFile`, `NtReadFileScatter`, `NtWriteFileGather`): routine(context,
    I/O status block, 0) queued on the caller when the transfer succeeded and both routine and context are given, the
    routine's low bit dropped, as the reference.
  - Kernel APCs (`KeInitializeApc`, `KeInsertQueueApc`, `KeRemoveQueueApc`): the 40-byte KAPC laid out and queued as
    the reference (once, on a thread that may queue APCs). At delivery the kernel routine runs first as kernel(apc,
    &normal routine, &context, &arg1, &arg2) and may change or clear the normal call that follows. User-mode ones run
    at alertable waits like the others; kernel-mode ones (those without a normal routine first) run at the start of
    every wait or delay, which the reference never does.
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

## Priorities and the MVP

The order of work comes from the titles that feed the database (`tools/build_db.py`): for each system call, how many
known titles import it and, where a title's generated code is at hand, how many call sites it has. Two of the titles
are known only by their import list, read from a XenonRecomp port's own minimal kernel (Sonic the Hedgehog (2006) from
MarathonRecomp, Test Drive Unlimited from TDURecomp; `titles/<title>/imports.json`, names only): a kernel that small runs
the game, so those lists say what a native port needs first. Small ReXGlue recomps do not publish their generated code,
so their import tables are not available without the game.

**MVP reached for Sonic the Hedgehog (2006)**: all 178 of its system calls outside graphics and audio are native (2026-11-01),
and with the kernel side of graphics and audio, all 200 of its system calls (2026-11-01). What it still needs from
outside the layer is the backends: the GPU command processor and renderer, the audio output and XMA decoding.

**MVP reached for Crackdown**: all 248 of its system calls are native (2026-11-03), graphics and audio included on
the kernel side; its graphics already run through any360::gfx (`layer/host/rexglue`).

**MVP reached for Test Drive Unlimited**: all 278 of its system calls are native (2026-11-03), graphics and audio
included on the kernel side.

**MVP reached for The Simpsons Game**: all 317 of its system calls are native (2026-11-03), graphics and audio
included on the kernel side. Every known title has now reached the MVP, and every system call the four titles
import (374) is native.

**MVP**: one title with every system call except the graphics and audio ones native, so it can run on `any360::sys`
with only those left to the reference. The progress page shows the MVP per title; the closest title is the target, and
the calls the four titles share come first.

Next for the layer, in this order:

1. **Object manager and scheduler, continued**: pre-emptive suspension, freeing kernel objects, `KeGetCurrentThread`-style queries.
2. **Memory, continued**: `NtProtectVirtualMemory`, `MEM_RESET`, the executable's image range as a queryable heap,
   `ExAllocatePool*`, `MmCreateKernelStack`, `KeLockL2` / `KeUnlockL2`.
3. **Files, continued**: STFS / XContent packages (saves, DLC), I/O completion ports, APC completion.
4. **XAM**: content enumeration and creation over the file system, the user profile and sign-in, input, the UI
   dialogs and the Live-offline answers.
