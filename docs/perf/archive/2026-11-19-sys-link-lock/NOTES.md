# FileSystem's symbolic links under a lock

Follow-up to 2026-11-18-sys-object-bookkeeping, which guarded the mount list: the links (`links_`, DOS name to device
path) and the link objects (`link_objects_`, a symbolic link handle's target) were read and written without a lock,
while a title links content on one thread (XamContentCreate's `Files().Link`) and resolves paths on others
(every open goes through `Canonical`), and a close hook erases link objects on whichever thread closes.

Now one `std::shared_mutex` (`links_mutex_`) guards both: shared for the look-ups (`Canonical`,
`OpenSymbolicLink`'s search, `QuerySymbolicLink`), exclusive for `Link`, `Unlink`, `DeleteSymbolicLink`, `Reset` and
the link object's insertion and removal. It is never held across a call into the object manager
(`OpenSymbolicLink` copies the target, allocates the object, then takes the lock to record it).

Test: `LinkConcurrencyTests` (sys_object_tests): one thread mounts, links, unlinks and unmounts eight content devices
in a loop while three threads each open and close a file through `game:` and open, query and close the `\??\game:`
link object 300 times; all 900 opens and 900 queries must succeed. On the sources before the change the same test
failed 2 runs of 5 (opens or queries lost to the race); after, 0 of 8.

Measured on 2026-11-19 with `sys_step12_bench 9`, before (22bf508's sources with this change's bench row) and after
alternated three times each on cores 0-7. Raw output: `before_run1-3.txt`, `after_run1-3.txt`.

| row | before ns | after ns | B / allocations (both) |
|---|---|---|---|
| Files Knows (link + 17 mounts: the path alone, no host file) | 195.5-201.2 | 193.4-201.1 | 64 / 2 |
| Files NtCreateFile+NtReadFile 16+NtClose (host file) | 120 064-124 321 | 117 935-122 028 | 1422 / 14 |

The locks cost nothing measurable (an uncontended shared lock is a few ns). The two allocations of the path look-up
are older: the lower-case copies of the path `Canonical` and `MountOf` make (a later pass could compare without
copying). There is no original or host column: the reference keeps its links in host objects.
