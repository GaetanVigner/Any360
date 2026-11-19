# Step 12, the rows under 1x: the object close path and the mount

The owner asked for the calls slower than the original to be optimized. Measured on 2026-11-19 (AMD Ryzen 7 7800X3D,
cores 0-7, the machine otherwise quiet) with `sys_abi_bench 9` and `sys_step12_bench 9`, three times each
(`sys_abi_bench_run1-3.txt`, `sys_step12_bench_run1-3.txt`). The runs before are
`../2026-11-19-sys-step12-user/sys_abi_bench_run1-3.txt`. Each change was also timed apart, without the ABI, by
`objprof.cpp` (a diagnostic program built against `any360_sys.lib`; the binaries before each change kept side by side,
three rounds in `objprof_runs.txt`).

## What was under 1x, and what was done

The rows under 1x in the three runs before, sorted:

- **The object lifecycle** (made faster): `XamProfileCreateEnumerator + NtClose` and
  `XamUserCreateTitlesPlayedEnumerator + NtClose`, 0.6x. Every handle close of every title goes through it.
- **The mount** (made faster, still under 1x): `XamProfileOpen + XamProfileClose`, 0.2-0.3x.
- **Real work against a stub that does nothing** (left as they are): the hashes and ciphers (`XeCryptMd5*`, `Sha256`,
  `HmacSha*`, `HmacMd5`, `DesCbc`, `BnQwNeModMul`, `BnQwBeSigFormat`), `LDIDecompress` and the content calls on the
  host's file system. They were compared with what the host offers and brought to its level in
  `2026-11-18-sys-step12-opt` and `2026-11-19-sys-step12-content` (the content enumerator 1.07-1.13x the host's own
  listing here). `XamAllocEx + XamFree` (0.5-0.6x) zeroes 12 KB of pages, which the call promises, and about 80 of its
  105 ns are that `memset`. It is already 17-25x `VirtualAlloc + VirtualFree`.

| row (through the ABI) | original ns | before ns | after ns | speed before | speed after | B, allocs before | after |
|---|---|---|---|---|---|---|---|
| XamProfileCreateEnumerator + NtClose | 66-69 | 106-111 | 59-65 | 0.6x | 1.0-1.1x | 0, 0 | 0, 0 |
| XamUserCreateTitlesPlayedEnumerator + NtClose | 68-69 | 112-113 | 63-64 | 0.6x | 1.1x | 0, 0 | 0, 0 |
| XamProfileOpen + XamProfileClose | 133-135 | 523-635 | 440-524 | 0.2-0.3x | 0.3x | 568, 10 | 440, 6 |

## The close path (every object of every title)

Timed apart (`objprof`, ns a cycle, the fastest of 9 runs, three rounds):

| cycle | before | A: one lock in Close | A + B: plain objects |
|---|---|---|---|
| an XAM enumerator made and closed | 102-104 | 87-92 | 54-61 |
| an object with no hint, a handle opened and closed | 93-94 | 77-84 | 80-85 |

- **A, one locked section in `Close`**: the handle found, closed and its last-ness known under one hold of
  `lifetime_mutex_`, instead of `Find`'s nested lock and a second lock in `Closed`. The close hooks still run outside
  the lock and before `Release` (once the block is freed, another thread may reuse its address). 12-15 ns. Kept.
- **B, plain objects**: a close used to run every close hook (the file system's: two locks and two map look-ups,
  17 ns; XAM's) and, at release, the timer and port tables (two more locks) and the free hooks (the scheduler's: a lock
  and a look-up), whatever the object was. `AllocatePlainObject(size, close_hook)` marks an object that only one hook
  keeps state for (or none). The mark sits in the low 4 bits of its entry in `freeable_`, whose sizes are 16-aligned,
  so it costs no extra look-up. Its last close runs that hook alone, and its release skips the tables and the free
  hooks. XAM's enumerators and notification listeners (its close hook) and session handles (no hook) are plain.
  Setting a timer on an object clears its mark. 30-35 ns more on an enumerator; an object with no mark pays one map
  look-up more (about 3-5 ns, within the noise of the rounds). Kept.
- **Safety net**: the hooks return whether they held state for the object. With `ANY360_VERIFY_OBJECT_HINTS` set,
  which `layer/sys/test.ps1` does for every test executable after the build, every hook and table still runs for a
  plain object, and one that held state for it stops the process with the hook's name. `sys_object_tests`
  (`PlainObjectTests`) checks both modes:
  - the owner's hook alone, and only on the last close;
  - the memory going back to the heap;
  - an object with no mark still running every hook;
  - a timer with an APC set on a plain object, cancelled at its close (the mark cleared), with nothing queued after.

  Writing it found a stale XAM enumerator from an earlier test at a reused address: `Fresh()` resets the object heap
  but not XAM's lists, which a title never does. The test resets XAM, as 29 others already do.

## The mount (`XamProfileOpen + XamProfileClose`, `XamContentCreate` / `XamContentClose`, every Mount and Link)

Timed apart (`objprof`): open and close 511-746 ns and 10 allocations before, 400-405 ns and 6 after. Mount and
Unmount went from 206-279 ns and 7 allocations to 137-140 ns and 4. Link and Unlink went from 136-137 ns and 3
allocations to 97-98 ns and 2.

- The device name a mount is given is lowered in place and moved into the mount entry: no `Lower` copy, and no second
  copy into the entry.
- `Unmount` compares names ignoring case, without a lowered copy.
- The longest-first order is kept by inserting at the right place, not by a `stable_sort` after `push_back` (its
  buffer).
- `Link` lowers its name in place and moves its target into the table.

The 6 allocations left are the mount itself: the shared entry, its name and host path, the link's node and target, and
the name `XamProfileClose` builds to unmount. A title opens its profile once or twice a session; the pair stays at
0.3x against a stub that mounts nothing, and no profile-only cache was made for it.

Nothing here was run in a game: the close path changed for every title, checked by the suite only.
