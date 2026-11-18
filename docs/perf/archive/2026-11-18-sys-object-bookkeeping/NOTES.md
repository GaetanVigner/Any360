# The object manager's bookkeeping without host allocations; stable mount entries

Two generic fixes in `any360::sys` found while porting step 12's profile calls (2026-11-18-sys-step12-profile).
Measured on 2026-11-18 with `sys_step12_bench 9`, the build before (`HEAD` 0245923's sources with this change's bench
rows) and after alternated three times each on cores 0-7. Raw output: `before_run1-3.txt`, `after_run1-3.txt`.

## 1. ObjectManager: no host heap in a create / close cycle

Before, every object created freed-on-close cost one node in `freeable_` (an `std::unordered_map`), every Ob
reference one node in `references_`, a block freed below the heap's top one node in `free_blocks_` (an `std::map`),
and `Release` / `Closed` scanned the whole handle table for an open handle on the object.

Now (`include/any360/sys/u32_map.h`, `src/objects.cpp`):

- `U32Map`: a flat 32-bit to 32-bit map (open addressing, linear probing, backward-shift removal, Fibonacci hash for
  aligned guest addresses) that allocates only when it grows past half full, and keeps its storage on `Clear`.
  It holds the freeable sizes, the Ob references and a new count of open handles per object.
- `Release` and `Closed` look the object's open handles up in that count instead of walking the table.
- The free blocks are a vector of (address, size) sorted by address, its capacity reserved; first fit, the split
  block's rest kept in place, coalescing on both sides unchanged.
- `AddHandle` still takes the lowest closed slot, starting from a remembered lowest-free index.

Behaviour is unchanged: same handles, same addresses, same lifetime rules (tests: `U32MapTests`,
`ObjectBookkeepingTests`, plus the existing `Lifetimes` and `References`).

| row (sys_step12_bench) | before ns | after ns | before B / allocs | after B / allocs |
|---|---|---|---|---|
| XamProfile Create+Enumerate x2+Close | 141.7-289.2 | 120.6-136.1 | 24 / 1 | 0 / 0 |
| Objects NtCreateEvent+NtClose | 104.9-195.8 | 84.1-98.6 | 24 / 1 | 0 / 0 |
| Objects Create+ObRef+ObDeref+Close | 176.1-322.8 | 117.9-121.9 | 48 / 2 | 0 / 0 |
| Objects 2 creates, lower closed first | 246.7-466.1 | 170.6-176.9 | 88 / 3 | 0 / 0 |
| Objects NtCreateEvent+NtClose, 64 open | 217.5-332.8 | 82.9-85.3 | 24 / 1 | 0 / 0 |
| Files NtCreateFile+NtReadFile 16+NtClose (host file) | 121 597-129 289 | 120 628-156 726 | 1438 / 15 | 1422 / 14 |

(ns: the fastest of 9 runs, range over the three launches; the first "before" launch was slower across the board.)
The cycle with 64 handles open went from 2.6x the empty table's cost to the same cost: the close no longer scans.
Over the whole bench, host memory a call fell from 5.06 B / 0.132 allocations to 2.18 B / 0.022 allocations. The other
rows did not move beyond ~1 ns of noise. There is no host or original column: the reference keeps this state in host
objects and the stub the title called makes none.

Fixed memory cost: three `U32Map`s of 256 / 64 / 256 entries (2 KB + 0.5 KB + 2 KB) and 64 reserved free-block slots
(0.5 KB), allocated once at start-up.

## 2. FileSystem: an open file keeps its mount entry

`Open::mount` was a `const Mounted*` into `mounts_`, a vector that `Mount` erases from, appends to and sorts: any
`Mount` while a file was open could leave it pointing at another device's entry or freed memory. Now the list holds
`std::shared_ptr<const Mounted>` entries (never changed once listed) and an open file keeps a reference to its own.
The list is guarded by a `std::shared_mutex` (a title mounts content on one thread while others open files).

Choices made with it:

- An open handle keeps the entry it was opened on: after its device is remounted read-only it still writes (it was
  opened writable), after an `Unmount` it still reads and closes. New opens see the new entry.
- A rename's "same device" check compares the device names (before: the entry pointers).

Test: `MountStabilityTests` opens a file, mounts 40 other devices (the list reallocated and sorted again), remounts
the file's device read-only twice, unmounts another, then writes, reads back, queries the volume and closes; a new
open for writing is then refused; a read handle survives the device's unmount.

Cost: the file open row above is the host file's open (about 120 us) and is at par; the open copies a `shared_ptr`
and takes a shared lock (a few ns). It does one allocation fewer (the object manager's node).
