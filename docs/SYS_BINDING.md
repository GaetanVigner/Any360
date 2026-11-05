# Running a title on any360::sys: the full switch

The goal (decided 2026-11-02, confirmed 2026-11-05): a recompiled title runs its whole kernel on any360::sys —
threads, objects, memory, files, XAM, audio — with ReXGlue left as the oracle and as the loader. Today only the graphics
calls are bound (`layer/host/rexglue`, `ANY360_REXGLUE_GRAPHICS_EXPORTS`). This is the plan to get the rest there,
first on Crackdown (`crackdown-any360-test`), one subsystem at a time, each tested in game before the next.

## What exists

- **602 system calls and variables** in `layer/sys/ppc/exports.cpp` (`kExports`, `kVariables`), every call the
  reference implements, each checked by scenario tests and, where the reference can be compared, by
  `sys_io_tests` / `sys_runtime_tests`.
- **The override mechanism**: the executable defines `__imp__<name>`, which the linker prefers to the runtime
  library's; until Any360 is started it calls the runtime's own export, looked up by name, so one build runs either
  way (`ANY360_REXGLUE_BIND_GRAPHICS`).
- **What ReXGlue still owns when the title starts**: the guest address space and its heaps (the XEX image at
  0x82000000, its system heap, thread stacks), the main thread (its KTHREAD, PCR, TLS), the handles it created while
  loading, the virtual file system (`game:`, the cache partition), the audio system and its XMA decoder.

## The obstacles, and how each is met

1. **Two owners of one handle range.** Both object managers start at 0xF8000000. Any360's will take slots from a
   range of its own (a `SetHandleBase`, for example 0xF8100000 upward) so a handle names its owner; `NtClose`,
   `NtDuplicateObject`, waits and `ObReferenceObjectByHandle` route a handle to whichever manager made it while both
   live. Titles only compare handles with what they were given, so the base needs no other care.
2. **One address space, two allocators.** Any360's `MemoryManager` takes over every allocation call, starting from
   ReXGlue's page map at the switch (the image, the runtime's own blocks, the stacks it made are marked as taken),
   and gives the object manager a kernel heap carved from it. ReXGlue's own later allocations (its internal
   structures) come from its system heap, which Any360 leaves out of its heaps.
3. **The main thread is ReXGlue's.** Its KTHREAD and PCR follow the console's layout, which any360::sys reads through
   r13 already; the scheduler adopts it (a `Scheduler::AdoptCurrentThread`) so suspension, APCs and thread ids work
   for it as for the threads Any360 creates. Guest code runs through a dispatcher the binder provides on top of
   ReXGlue's `FunctionDispatcher` (up to eight arguments, `Dispatcher` in `scheduler.h`).
4. **Files the runtime mounted.** The binder mounts the same host directories on Any360's file system (`game:` on the
   title's files, the cache partition, the content root for saves) before the first file call.
5. **XMA decoding.** any360::sys keeps the XMA contexts; decoding them is an `XmaBackend`'s job, which does not exist
   yet. Until one does, audio stays on ReXGlue (`XAudio*`, `XMA*` keep the runtime's exports) — the one group the
   switch leaves behind on purpose.

## Steps

Each step adds exports to the binder behind a setting of its own (`any360_sys_<group>`, off by default) so a
regression is one switch away from the runtime's behaviour, and ends with an in-game check on Crackdown (title screen,
a save loaded, ten minutes of play) batched into as few launches as possible.

| Step | Groups | What has to be true first |
|---|---|---|
| 0 | the binder: `ANY360_REXGLUE_SYSTEM_EXPORTS(group)` generated from `kExports`, the dispatcher, variable imports through `VariableAddress` | nothing |
| 1 | stateless calls: `Rtl*` strings and time, printf family, crypto, locale, `XGet*` information, `DbgPrint` | step 0 |
| 2 | files: `Nt*File`, `NtQuery*`, the Win32 file calls, symbolic links | the mounts (obstacle 4), the handle routing (obstacle 1) |
| 3 | objects and waits: events, semaphores, mutants, timers, completion ports, `Ob*` | the handle routing |
| 4 | threads: `ExCreateThread`, `Ke*Thread`, TLS, APCs, DPCs, critical sections, spin locks | the adopted main thread and the dispatcher (obstacle 3) |
| 5 | memory: `NtAllocateVirtualMemory`, `Mm*`, pools, encrypted memory, the kernel heap | the page map taken over (obstacle 2) |
| 6 | XAM: users, content (saves), notifications, enumerators, UI, network | steps 2 and 3 |
| 7 | audio: `XAudio*`, `XMA*` | an `XmaBackend` that decodes |

After step 6 every system call but audio is Any360's; after step 7 ReXGlue only loads the image.

## Step 0: done (the binder)

- `layer/host/rexglue/include/any360/host/system.h` and `src/system.cpp`: `SysGroup` (stateless, files, objects,
  threads, memory, xam, input, audio), `EnableSystemGroup`, `CallSystemExport` (Any360's call when the group is on or
  the runtime lacks the export, the runtime's otherwise), `StartSystem` (any360::sys on the runtime's guest memory, a
  kernel heap from its system heap, the dispatcher, the variable imports), `DispatchGuest` (guest code through the
  runtime's function table, up to eight arguments, on a context of its own), `BindVariables` (the executable's
  variable imports rewritten to any360::sys's variables).
- `system_exports.inc`, generated by `tools/gen_host_binding.py` from `exports.cpp` and the database: 508 overrides
  (every call but the video group's, which the graphics binding has), each skippable with
  `ANY360_REXGLUE_SKIP_<name>` where the title hooks the call itself.
- `host_system_tests` (in `layer/sys/test.ps1`): every override linked into a "title" inside the runtime; groups off
  reach the runtime's exports, on reach any360::sys's (calls whose answers differ on the two sides); the dispatcher
  runs guest code with eight arguments.

## Step 1: done (the stateless calls in Crackdown)

Crackdown's branch `any360-sys-test` (`src/any360_system.cpp`) takes every override, starts the system binding in
`OnPostSetup` and switches the groups the `any360_sys_groups` setting names (`any360_sys_trace` logs the first call
of each call that goes to any360::sys). With `stateless` and Any360's GPU it ran four minutes of play with no crash
(2026-11-05); Crackdown makes 16 stateless calls (`RtlImageXexHeaderField`, `ExGetXConfigSetting`, the printf
family, `XeCryptSha`, `XeKeysConsolePrivateKeySign`, the time and string conversions...).

What the game found that the tests could not:

- The import table is read-only once the image is loaded: `BindVariables` makes the page writable for the store.
- The title's settings (module handle, XEX header, title id, module name) must come from the loaded module:
  `StartSystem` fills them, or `XexExecutableModuleHandle` and `RtlImageXexHeaderField`'s callers read nothing.
- A variable belongs to the group whose calls read it: bound with every variable, the object types no longer matched
  the runtime's `ObReferenceObjectByHandle` and the title read through a null object. `BindVariables` (called after
  `EnableSystemGroup`) now binds only the variables of the groups switched (`VariableGroup`).

## Step 2: done (the files in Crackdown)

The runtime and any360::sys now both hold handles and paths, so the overrides that take one route by owner instead of
by group (`gen_host_binding.py`: `HANDLE_ROUTES`, `PATH_ROUTES`, `BOTH_ROUTES`):

- A handle goes back to the manager that made it: any360::sys's start at `SystemOptions::handle_base` (0xF8100000,
  `ObjectManager::SetHandleBase`, `OwnsHandle`), the runtime's below. `NtClose`, `NtReadFile`, the waits and the other
  56 calls that take a handle follow it whatever the groups say.
- A path goes to any360::sys when its files are on and its file system knows the device (`FileSystem::Knows`,
  `KnowsAttributes`): `StartSystem` mounts `SystemOptions::mounts` as the runtime does (Crackdown: `game:` and `d:` on
  the title's files, `\Device\cache0` on the cache partition). Devices the title or the runtime add on their own
  (Crackdown's button glyphs) stay the runtime's.
- `ObCreateSymbolicLink` and `ObDeleteSymbolicLink` run on both sides, so a link the title makes resolves on either.

With `stateless,files` and Any360's GPU Crackdown reaches play at 240-280 FPS (2026-11-05); its file calls are
`NtCreateFile`, `NtOpenFile`, `NtReadFile`, `NtWriteFile`, `NtQueryInformationFile`, `NtSetInformationFile`,
`NtQueryFullAttributesFile`, `NtQueryDirectoryFile`, `StfsCreateDevice`, `StfsControlDevice`,
`IoDismountVolumeByFileHandle` and the symbolic links.

What the game found that the tests could not:

- A file read's event is the runtime's while the objects are: the read failed with `STATUS_INVALID_HANDLE` and the
  title waited for ever. `ObjectManager::SetForeignHandles` lets `NtSetEvent` (and the files' event checks,
  `EventHandleValid`) reach the other kernel's events for handles outside any360::sys's range.
- On Windows the runtime maps guest addresses from 0xE0000000 up 0x1000 bytes further on the host (64 KB mapping
  granularity, `PhysicalHostOffset`), and the title's code adds the same. `Guest` did not: reads into physical memory
  landed 4 KB off and the title parsed garbage. `Guest::host` now adds `g_physical_host_offset`, which `StartSystem`
  takes from the runtime's 0xE0000000 heap.
- Finding those took a trace of every call: `SystemOptions::trace_every_call` (Crackdown: `any360_sys_trace_all`)
  logs each call that goes to any360::sys with eight arguments, its result and, for a path call, the path.

Still the runtime's while only the files are switched: an I/O APC (queued on any360::sys's scheduler, which runs no
runtime thread) and a completion port the runtime made; Crackdown uses neither.

## Step 3: done (the objects in Crackdown)

With `stateless,files,objects` Crackdown loads and plays (2026-11-05, no hang in four minutes, the same frame rate as
with the files alone);
its object calls are the events (`Nt*Event`, `Ke*Event`), semaphores, mutants, `NtDuplicateObject`, `NtClose`,
`ObDereferenceObject` and the single and multiple waits.

What the game found that the tests could not:

- The dispatcher was not thread-safe: a `ReleaseSemaphore` and a wait taking the count on two threads each read the
  count and wrote it back, so a release could be lost and a loading worker waited for ever (an intermittent hang in
  the loading screen). The handle table had the same flaw (two threads creating events could get one slot, and a
  growing table moved the entries `Find` had handed out). `ObjectManager` now keeps a dispatcher lock, as the kernel
  does, for signal states, timers and the waits' tests, and the handle table (a deque) under `lifetime_mutex_`.
  The waits sleep on a condition the routines signal instead of spinning, and look again every millisecond for states
  written elsewhere (the title's inline `stw 0,4(r3)` resets, the other kernel). `sys_object_tests` has a
  four-producer, four-consumer test.
- Waking every wait on each signal (one condition for all) cost 17-22% of the frame rate: a dozen of the title's
  threads woke and queued on the lock for each event. A wait now sleeps on its thread's own condition, listed under
  the objects it waits on, and a signal wakes only those. Same city route, files only against files and objects:
  262 and 270 FPS (2026-11-05).
- The object type variables (`ExEventObjectType`...) stay the runtime's: a title only hands them back to
  `ObReferenceObjectByHandle` and its kin, which any360::sys ignores and the runtime checks for the handles it keeps.
- A redirection the title sets up on the runtime's VFS (Crackdown's HD button glyphs over `game:\Local\<language>`)
  needs the same mount on any360::sys's file system once the files are switched, and `FileSystem::Canonical` joined
  `game:\Local` into a double separator, which hid a mount over a folder of a device.
- `trace_every_call` reports, besides any360::sys's calls, each runtime call handed one of any360::sys's handles, and
  the start of every wait (a wait that never returns shows).

## Who does what

- `layer/sys` (the calls, `SetHandleBase`, `AdoptCurrentThread`, the page map import): the sys session.
- `layer/host/rexglue` (the binder, the dispatcher, the mounts): the session that owns the host layer, or the sys
  session with its agreement.
- `crackdown-any360-test` (the settings in the title, the in-game checks): the Crackdown session.
