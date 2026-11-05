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

What step 1 needs from the title (the Crackdown session): include `system.h` and `system_exports.inc` with a skip for
each `__imp__` it already hooks, call `StartSystem` once the title is loaded, and a setting naming the groups to
switch (`any360_sys_groups = "stateless"`).

## Who does what

- `layer/sys` (the calls, `SetHandleBase`, `AdoptCurrentThread`, the page map import): the sys session.
- `layer/host/rexglue` (the binder, the dispatcher, the mounts): the session that owns the host layer, or the sys
  session with its agreement.
- `crackdown-any360-test` (the settings in the title, the in-game checks): the Crackdown session.
