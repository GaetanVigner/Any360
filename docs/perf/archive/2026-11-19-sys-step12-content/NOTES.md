# Step 12, ninth batch: 10 content, network and other calls

The calls Xenia implements and the reference runtime stubs (docs/SYS_STUBS.md): `XamContentCreateEnumeratorInternal`,
`XamContentGetMetaDataInternal`, `XamContentSetThumbnailInternal`, `XamContentRegisterChangeCallback`, `XamReadString`,
`NetDll_WSAStartupEx`, `NetDll_XNetStartupEx`, `XamNetworkStorageUserHasPrivilege`,
`XamLoaderRegisterLaunchRequestCallback`, `ObLookupAnyThreadByThreadId`. Measured on 2026-11-19 (AMD Ryzen 7 7800X3D,
cores 0-7) with `sys_abi_bench 9` and `sys_step12_bench 9`, three times each (`sys_abi_bench_run1-3.txt`,
`sys_step12_bench_run1-3.txt`; run 3 of the ABI bench and run 2 of the other had the machine busier, hence the ranges).
A second set on a quiet machine (`*_run4-6.txt`, 11:45) gives the same picture: the answers 2.6-15 ns (14-27x), the content rows 59-63 us (metadata), 105-114 us (enumerator), 183-199 us (thumbnail), the enumerator 1.07-1.10x the host floor on the quiet passes; one pass had a slow spike on its 32-package enumerator row (0.71x, 1.57 ms against 1.07-1.08 ms in the other two): the file rows swing with the host's background disk work (Windows Defender scans the new files). The content rows work on packages made as a title makes them (`XamContentCreate`: the folder and its header), in the
temporary folder; every new ABI row's result is checked after its timing (none differed).

Reused, not recoded: the content packages (`ContentItems`, `ContentPath`, `ContentHeaderPath`, `ContentSetThumbnail`,
the enumerators), `WsaStartup` and `XNetStartup`, `LookupThreadById`, the overlapped completion (`Finish`).

Against the original (the reference's stub, 66-71 ns a call on the quiet runs, doing none of the work):

| call | original ns | Any360 ns | speed | B, allocs a call |
|---|---|---|---|---|
| XamLoaderRegisterLaunchRequestCallback | 70-71 | 2.9-3.1 | 23-25x | 0, 0 |
| XamContentRegisterChangeCallback | 70 | 2.9-3.1 | 23-24x | 0, 0 |
| XamNetworkStorageUserHasPrivilege | 69-70 | 2.9-3.0 | 23-24x | 0, 0 |
| XamReadString | 67 | 4.4-4.6 | 15x | 0, 0 |
| NetDll_XNetStartupEx | 70 | 5.3-5.5 | 13x | 0, 0 |
| NetDll_WSAStartupEx | 69-70 | 7.7-8.6 | 8-9x | 0, 0 |
| ObLookupAnyThreadByThreadId (unknown id) | 68-70 | 10.7-10.9 | 6.4x | 0, 0 |
| XamContentGetMetaDataInternal | 69-70 | 61-62 us | - | 944 B, 9 |
| XamContentCreateEnumeratorInternal, 2 packages, + NtClose | 70-71 | 113-114 us | - | 3366 B, 30 |
| XamContentSetThumbnailInternal, 4 bytes | 69-70 | 192-194 us | - | 608 B, 6 |

The three content rows do the work the stub skips (it answers nothing): the package's folder looked up and its
header file read, the type's folder listed and every header read, the thumbnail file written. Their time is the host
file system's (file opens, Windows Defender's scan of new files); the bytes are the paths and file names.

Against the host (`sys_step12_bench`, the fastest of 9 runs, three runs): the enumerator against the same folder
listed and the same headers read with nothing else (std::filesystem's iterator, fopen / fread / fclose), the floor of
this work on the host:

| work | packages | Any360 us | host floor us | speed |
|---|---|---|---|---|
| XamContentCreateEnumeratorInternal + close | 2 | 109-168 | 115-186 | 1.05-1.11x |
| | 32 | 1089-1684 | 1162-1952 | 1.06-1.17x |
| XamContentGetMetaDataInternal | 2 / 32 | 58-92 | - | - |
| two folder dates (what a listing cache's check costs) | - | 40-71 | - | - |

- **A listing cache** (keep each type's packages, check the folders' dates) was weighed and not made: the enumerator is
  already at the host's floor, and the cache's own check (two folder dates, 40-71 us) is a third to two thirds of the
  whole call, for a call titles make on a save screen; its risks (changes inside the same date tick, headers rewritten
  in place) are not worth it.
- **`std::ifstream` to `fopen`** for the header reads: no allocation less (the 30 are the paths and names) and no
  measurable time: not kept in the shared listing; the new metadata call reads with `fopen`.
- The answers' sources, checked in Xenia: `XamContentGetMetaDataInternal` gives a folder package's header as Xenia's
  `ContentPackageDirectory` reads it (an XCONTENT_AGGREGATE_DATA: type, version 2, one data file, STFS, the header's
  title id and display name; all 0 without a header), and fails without touching the overlapped;
  `XamNetworkStorageUserHasPrivilege` composes Xenia's `XamUserCheckPrivilege`, which grants nothing to a profile not
  signed in to Xbox Live: 0 for the local, offline profile.
- Not covered: five of the calls are an adapter line over a function the tests call directly (their register
  placement is checked by reading only); a title calling these in game; a Linux build.

Checked by `sys_object_tests` (`XamStep12NinthTests`): the internal enumerator on devices 0, 1, 2 and two unknown
ones, no handle pointer, the size written; the metadata of a package without a header, with an aggregate header, with
a short one, through an overlapped, of a missing package (the overlapped untouched); the thumbnail kept; the callbacks
kept and cleared; XamReadString's empty string, its sizes and errors, through an overlapped; the privilege for every
user; the Ex startups.
