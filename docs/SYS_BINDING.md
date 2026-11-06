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

## Step 4: done (the threads in Crackdown)

With `stateless,files,objects,threads` Crackdown loads and plays (2026-11-05, 270-291 FPS on the city route at
~2,250 draws, as with the objects alone); its 29 thread calls include `ExCreateThread`, the TLS, critical sections,
spin locks and IRQL, `KeDelayExecutionThread`, affinity and priority, `ExTerminateThread`.

What the game found that the tests could not:

- The runtime answers its calls only on its own threads: the first `XMsgInProcessCall` (XAM, still the runtime's)
  from a thread any360::sys made read a null current thread. `Scheduler::SetThreadRunner` lets a binder say how the
  host threads that run guest code run it; the ReXGlue binder runs each of them (the threads, the DPC worker) inside an
  `XHostThread`, so the runtime sees a thread of its own while the guest code keeps any360::sys's KTHREAD and PCR.
  `host_system_tests` checks it. The main thread and the threads made before the switch stay the runtime's; nothing in
  Crackdown needed any360::sys to adopt them.

## Step 5: done (the memory in Crackdown)

With `stateless,files,objects,threads,memory` Crackdown loads and plays (2026-11-05). On the city route at 2,000-2,600
draws, the same route driven twice: 259 FPS with the memory group, the level of steps 3 and 4 (the run without it, 215,
shared the machine with a build): the shared allocator costs nothing measurable. Its memory calls: `NtAllocateVirtualMemory`,
`NtFreeVirtualMemory`, `NtQueryVirtualMemory`, `MmAllocatePhysicalMemoryEx`, `MmFreePhysicalMemory`,
`MmGetPhysicalAddress`, `Mm{Query,Set}AddressProtect`, `MmQueryStatistics`, `XamAlloc`, `XamFree`.

One guest address space, two allocators: the runtime goes on allocating its own structures (its system heap, the
stacks of the `XHostThread`s, XAM's and audio's buffers). `MemoryManager::SetSharedOwner` gives the memory manager the
other allocator: every range it hands out is claimed there first (the ReXGlue binder commits it read-write in the
runtime's heap, which keeps the host pages accessible) and released there with it; a refused claim makes the binder
mark what the runtime holds (`MarkForeign`, from `QueryRegionInfo`) and the search moves on. `StartSystem` imports the
runtime's whole page map first. This also covers any360::sys's own allocations (pools, strings) while the memory
group is still the runtime's. `host_system_tests` checks both directions.

What the game found that the tests could not:

- The memory manager had no lock (as the dispatcher before step 3): it has one now.
- A thread with no handle left frees itself as it ends; since step 4 it runs on an `XHostThread`, not on the
  `std::thread` the scheduler keeps, so `OnFree` took it for another thread and joined the thread waiting for it,
  holding the objects' lock: the loading screen hung. The scheduler records the thread that really runs `Run`.

## Step 6: done (XAM in Crackdown)

With `stateless,files,objects,threads,memory,xam` Crackdown loads the save the runtime wrote, keeps its options and
saves (2026-11-05). Its XAM calls: the messages and notifications, `XamTaskSchedule`, the network startup, the user
(`XamUserGetSigninState`, `GetName`, `GetXUID`, `Read/WriteProfileSettings`, the achievement enumerator), the content
(`XamShowDeviceSelectorUI`, `XamContentCreateEnumerator`, `XamEnumerate`, `XamContentCreateEx`, `XamContentClose`).

Either side reads what the other wrote: `SystemOptions::user_data_root` (the runtime's user data folder) gives
any360::sys's XAM the runtime's signed-in profile (its XUID and name) and its layout: content packages under
`<root>/<XUID>/<title id>/<type>/<name>`, each with its XCONTENT_DATA in `Headers/<type>/<name>.header` (the display name
the title gave), binary profile settings (a title's options) as `<root>/<title id>/profile/<profile>/<setting id>`.
`XamEnumerate` now goes by handle (an enumerator the runtime made stays the runtime's).

What the game found that the tests could not (the save was "corrupt" until the last):

- A profile setting written shorter than its maximum reads back with the length written, not the maximum.
- Through an overlapped structure with no disposition pointer, `XamContentCreateEx`'s disposition is the length.
- A file's allocation size is its size rounded to a 512-byte sector (not 16 KB), and its four times are the host's
  (creation, last access, last write).
- The one that mattered: a file opened without `FILE_SYNCHRONOUS_IO_*` (kernel32's `FILE_FLAG_OVERLAPPED`) answers
  `NtReadFile` / `NtWriteFile` with `STATUS_PENDING`, the transfer done and its I/O status block holding the success, as
  the reference does. Answered `STATUS_SUCCESS`, kernel32's `ReadFile` returns TRUE, and Crackdown's save loader then
  meets an overlapped structure whose length has been cleared: `GetOverlappedResult` reads 0 bytes, the save is
  "corrupt". The completion APC and port packet carry the I/O status block's success.
- Found by bisecting: `SystemOptions::runtime_calls` leaves named calls to the runtime though their group is on (the
  calls sharing their state with them must go along), and `trace_groups` narrows `trace_every_call` to some groups so a
  long run's log keeps the calls looked into.

## Step 7: done (audio in Crackdown)

With every group on any360::sys (`stateless,files,objects,threads,memory,xam,audio`) Crackdown plays with its sound
(2026-11-05): ReXGlue loads the image and answers nothing but the graphics and input the other bindings keep. Its audio
calls: `XAudioRegisterRenderDriverClient`, `XAudioGetVoiceCategoryVolumeChangeMask`, `XAudioSubmitRenderDriverFrame`,
`XMACreateContext`, `XMAReleaseContext`.

- **The decoder is any360::sys's** (`any360_sys_xma`, `xma.h`): an `XmaBackend` running the XMA hardware's context
  machinery (packets, frames spanning packets, loops, the output ring the title reads), ported from Xenia's XmaContext
  (BSD), each frame decoded by FFmpeg's xmaframes decoder — the FFmpeg the ReXGlue SDK ships (its `libavcodec` /
  `libavutil`, with the headers of its source: `ANY360_FFMPEG_INCLUDE`). A separate target, so the core needs no FFmpeg.
  `xma_tests` checks the machinery; decoding itself is checked in game (no stream ships with the repository).
- **The XMA registers are any360::sys's**: XAudio2 (linked into the title) drives the hardware directly at 0x7FEA0000,
  not through the `XMA*` calls: the context array's address, the kick, lock and clear registers.
  `AudioKernel::XmaReadRegister` / `XmaWriteRegister` model them; the ReXGlue binder points the runtime's MMIO range
  there (`LookupVirtualMappedRange`). The contexts live in the 64 KB-page view, where a physical address is the virtual
  one's low bits.
- **The output**: `StartAudio` (the binder, `ANY360_REXGLUE_AUDIO`) gives the audio kernel an SDL3 sink (5.1 floats to
  the default device, its queue pacing the clients) and pumps the clients' callbacks on a guest thread.

What the game found that the tests could not:

- A thread gets its hardware thread at creation, as the reference: the processor mask in the creation flags' top byte,
  else the next of the six in turn, in its KTHREAD and PCR. any360::sys left every thread on processor 0: XAudio's two
  mix workers, meeting by processor number, waited for each other for ever and no frame was ever mixed.

## Step 8: done (input in Crackdown)

With all eight groups on any360::sys Crackdown plays on a gamepad (2026-11-06; keyboard and mouse not checked). Its
input calls: `XamInputGetState`, `XamInputGetCapabilities`, `XamInputGetKeystrokeEx`, `XamInputSetState`.

- any360::sys answers as XInput does: a state's packet number moves only when the state changes; keystrokes are made
  from the state's transitions (buttons, triggers, the sticks' eight directions) with the console's repeat, user 0xFF
  taking the first user with one. `sys_object_tests` checks both.
- The source is the host's: the ReXGlue binder fills `XamHost` (connected, state, vibration) from the runtime's input
  system, so every driver it has (SDL, XInput, a title's own keyboard and mouse driver) still feeds the title. Any360's
  own platform layer replaces it at step 14.

## Step 9: done (no call reaches the runtime in Crackdown)

With all eight groups on, Crackdown makes no system call the runtime answers (2026-11-06: loading a save, playing,
saving, sound, gamepad and vibration). `trace_calls` now also reports the first time each call goes to the runtime and
why (its group off, a handle the runtime made, a path any360::sys did not answer); the run that started this step
listed ~2,900 such calls, all files:

- **Every path is any360::sys's** once the files group is on: it mounts what the runtime mounts and answers a device
  nobody mounted (Crackdown's Lua probes its search path with relative names; the cache code tries a `cache:` link
  before creating it) as the console would, with NT's statuses. Only a path relative to a directory the runtime
  opened would still be the runtime's; symbolic links are made on any360::sys's side alone.
- **The raw hard drive devices** (`\Device\Harddisk0\Partition0`, `Cache0`, `Cache1`), which titles' cache code
  opens, queries and formats: `FileSystem::MountNull` answers them as the reference does (opens, reads and writes
  succeed and move nothing); the ReXGlue binder mounts them.
- **Profile defaults**: the integer settings a new profile holds (vibration on, voice volume, auto aim, the gamer
  card's) answer the console's default until a title writes them. Without them Crackdown read vibration as off.

What stays the runtime's: the host (it loads the image, runs the threads inside `XHostThread`s, owns the window),
which step 13 replaces; the routing by handle owner stays as the fallback for a title that keeps runtime-made objects.

## Step 10: done (The Simpsons Game on the full binding)

The Simpsons Game runs with all eight groups on any360::sys (2026-11-06): menus at 30 FPS and levels at 60 as the
reference (also upscaled 2x with MSAA 4x on any360::gfx), sound, gamepad, saves, levels played through, no call reaching
the runtime, a clean exit. The second title needed no title-specific code in Any360; what it found was Any360's:

- **The binding is Any360's to set up** (`any360/host/settings.h`): the any360_sys_* settings, the mounts a console
  title expects, the audio hardware, a crash report for titles without their own, the stop at shutdown. A title's
  whole system binding is one file including `system_exports.inc` and one call (`StartSystemFromSettings`), plus
  `StopSystemFromSettings` when it exits through the runtime's shutdown. Crackdown moved onto it.
- **A commit zeroed pages already committed**: `NtAllocateVirtualMemory(MEM_COMMIT)` over a range whose first page was
  new cleared the whole range, wiping a heap's live blocks (the game's allocator then followed a null free-list link).
  Only the pages a commit makes new are zeroed now.
- **XMA decoding is synchronous with the kick**: Simpsons drives the `XMA*` calls and takes `XMAEnableContext`'s
  return as its output being ready; a background decode could write into buffers it had reused. No thread decodes
  behind the title any more.
- **Shutdown**: the audio pump and the decoder stop first, then the title's threads park in their next system call
  or wait (`BeginShutdown`) instead of touching the guest memory the runtime frees. The runtime then waits up to 10 s
  for those threads before exiting (to improve).
- The in-tree SDK's FFmpeg targets (`libavcodec`, `libavutil`) build the XMA decoder when the SDK is built with the title.

Seen but not the binding's: above 60 FPS (`frame_rate=0`) short button presses can be missed, as without Any360 (the
title's own frame pacing).

## After step 7: from one title to any title (the road to 100%)

Steps 0-7 put Crackdown's system calls on any360::sys, with ReXGlue still hosting it. Crackdown is the test bench; the
goal is that any title ported with ReXGlue's code generator runs on Any360 alone. Measured on 2026-11-06, that is far:

- **Calls**: 602 of the 2,914 exports (xboxkrnl 922, xam 1,735, xbdm 255) are native, every one the four titles import
  (374). 2,294 are stubs in the reference too (it never needed them) and 14 are missing there: a fifth title will meet
  some of them.
- **Proven in game**: one title, Crackdown, and only on the paths it takes (its XAudio2 drives the XMA registers, so the
  `XMA*` decoding path is unproven; it reads saves through overlapped I/O, so other file patterns are unproven).
- **The host is still ReXGlue's**: it loads the XEX, owns the address space and its heaps, creates the main thread and
  the objects made before the switch, runs every guest thread inside an `XHostThread`, dispatches guest code, catches
  MMIO accesses, mounts the devices Any360 does not, owns the window, the SDL event loop and the input drivers.

The rest, in steps of the same size as 0-7, each ending with an in-game check on more than one title:

| Step | What | Done when |
|---|---|---|
| 8 | **Input**: the `input` group on any360::sys with an input source the host fills (an SDL3 gamepad layer: state, capabilities, keystrokes, vibration, hot-plug, four users); keyboard and mouse for titles that map them | Crackdown plays on a gamepad with input on any360::sys |
| 9 | **No call reaching the runtime**: every path answered by any360::sys, the raw hard drive devices, the profile defaults (done, see Step 9; the main thread and the host threads stay the runtime's until step 13) | Crackdown boots and plays with no call reaching the runtime |
| 10 | **A second title on the full binding**: The Simpsons Game, all groups, the bugs it finds fixed in any360::sys (none in the title) | The Simpsons plays through its first level on any360::sys |
| 11 | **Third and fourth titles**: Test Drive Unlimited, Sonic 06 | both reach gameplay |
| 12 | **Paths no title took yet**: the `XMA*` decoding path, content packages (STFS saves, DLC, title updates), several signed-in users, the guide and its UIs (keyboard, message box, device selector) answered as a console would, Live answered as offline everywhere, xbdm as a retail console; each with scenario tests | sys tests cover each; the 2,294 reference stubs sorted into "answered like retail", "implemented", "never seen" |
| 13 | **Any360's own host** (`layer/host/native`): the XEX loader (sections, imports bound to any360::sys, TLS, entry), the guest address space reserved and owned by `MemoryManager`, threads on host threads without `XHostThread`, the guest-code dispatcher over the generated functions, MMIO through the platform's fault handler, the devices (disc image, extracted folder, STFS) | a title links no `rexruntime`: ReXGlue is only its code generator |
| 14 | **The platform layer**: window, event loop, gamepad, audio output (SDL3) and the GPU presenter outside ReXGlue, on Windows and Linux (the Steam Deck) | Crackdown and one other title run from `layer/host/native` on both systems |
| 15 | **Graphics on the same footing**: every title's GPU through `any360::gfx` (the GPU session's plan, docs/GFX.md), no `rexgpu-xenos` | the four titles render with any360::gfx only |
| 16 | **Porting a new title**: a converter served from GitHub (as AnyPS5's) and built into the cross-platform launcher being written alongside, taking the player's XEX to a running build with no title-specific code in Any360 (`pipeline/any360.py` underneath; a title repository holds only its manifest, hooks and patches), tried on a fifth title never run before | a new title boots from the converter with nothing added to Any360 but fixes |

Steps 8-9 are this binding's; 10-12 widen it; 13-15 replace the host; 16 is the measure of "any game". Stage 2 and 3
(decompilation, the portable codebase, docs/DESIGN.md) come after.

## Who does what

- `layer/sys` (the calls, `SetHandleBase`, `AdoptCurrentThread`, the page map import): the sys session.
- `layer/host/rexglue` (the binder, the dispatcher, the mounts): the session that owns the host layer, or the sys
  session with its agreement.
- `crackdown-any360-test` (the settings in the title, the in-game checks): the Crackdown session.
