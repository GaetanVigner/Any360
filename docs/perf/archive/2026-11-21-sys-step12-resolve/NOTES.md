# Step 12: XamContentResolveInternal on a package already resolved

The owner asked for `XamContentResolveInternal` to be optimized. It was 0.1x the original on a package already
mounted: 557-585 ns, 7 allocations and 528 B a call (`../2026-11-21-sys-step12-content-stubs/`). Measured on
2026-11-21 (AMD Ryzen 7 7800X3D, cores 0-7, the machine otherwise quiet) with `sys_abi_bench 9` and
`sys_step12_bench 9`, three times each (`sys_abi_bench_run1-3.txt`, `sys_step12_bench_run1-3.txt`).

| row | original ns | before ns | after ns | speed before -> after | B, allocs before -> after |
|---|---|---|---|---|---|
| XamContentResolveInternal (mounted again), through the ABI | 68-70 | 571-585 | 48-91 (48.0, 48.5, 91.3) | 0.1x -> 0.8-1.4x | 528, 7 -> 0, 0 |
| the same without the ABI (`sys_step12_bench`) | - | 553-620 | 49-52 | - | 576, 7 -> 0, 0 |

The first ABI run's 91 ns is alone: the two others, and the three runs without the ABI, are 48-52 ns.

**Where the time went.** Each call built the package's host path, with `std::filesystem::path` and its wide conversion
on Windows. It then built the device path from that path's parts and mounted it, keeping the entry when it was the same
mount. All of this was redone for a package already mounted.

**The fast path.** A package asked again, with no folder to make and no root name, under the current content root, now
takes a shorter route:
- The device path ("\Device\Content\<type>\<file name>") is written from the guest bytes into a buffer on the stack.
  The file name follows `ContentPath`'s rules: at most 42 characters, '/', '\' and ':' turned into '_', and a name of
  only dots and spaces refused.
- `FileSystem::IsMountedOn` (new, generic) checks under the mount list's shared lock that this device is mounted,
  writable, on the folder root / type / name. It compares the mount's host path with the root, a separator, the type,
  a separator and the name in place, so no path is built.
- The path is then copied out as before.

Anything else takes the full path: the first call, a folder to make, a root name, a non-ASCII name, the disc, or a
content root that has moved since the mount. Kept: 12x faster, no allocation, the same answers.

Checked by `sys_adapter_tests` (`ContentAdapters`):
- a package asked again gives the same path, through an overlapped too, and the same truncation;
- a name with ':' is cleaned as `ContentPath` cleans it;
- after the content root moves, the old mount is not taken as resolved: the package is mounted again on the new root's
  folder, and back on the old one when the root returns, its files resolving there each time.

Nothing here was run in a game.
