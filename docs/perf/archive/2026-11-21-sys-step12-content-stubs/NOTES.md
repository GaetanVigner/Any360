# Step 12, the calls Xenia stubs too: the 10 content calls

`XamContentGetAttributesInternal`, `XamContentGetDeviceVolumePath`, `XamContentGetLocalizedDeviceData`,
`XamContentGetLocalizedString`, `XamContentLaunchImageFromFileInternal`, `XamContentLaunchImageInternal`,
`XamContentLaunchImageInternalEx`, `XamContentResolveInternal`, `XamReadTile`, `XamReadTileEx`, answered as Xenia's
(xam_content.cc, xam_content_device.cc, xam_user.cc).

Measured on 2026-11-21 (AMD Ryzen 7 7800X3D, cores 0-7, the machine otherwise quiet) with `sys_abi_bench 9` (the
original through the PowerPC ABI) and `sys_step12_bench 9` (against the host where it does the same work), three times
each: `sys_abi_bench_run1-3.txt`, `sys_step12_bench_run1-3.txt`. The first measure, before the mount change below, is
in `before/`. Every ABI row's result is checked after its timing.

Reused, not recoded:
- the package's folder (`ContentPath`) and the device data (`ContentGetDeviceData`);
- the host stat the file calls use (`HostFileStat`: times, size and kind in one question);
- the mount and link (`Files().Mount`, `Link`; a new `HasLink`);
- the overlapped completions (`Finish`, `CompleteEx`);
- `XamContentLaunchImage`'s answer.

| call | original ns | Any360 ns | speed | B, allocs a call |
|---|---|---|---|---|
| XamContentLaunchImageFromFileInternal, Internal, InternalEx | 68-73 | 1.6-2.3 | 31-45x | 0, 0 |
| XamReadTile, XamReadTileEx | 67-71 | 4.6-9.3 | 8-15x | 0, 0 |
| XamContentGetDeviceVolumePath | 70-73 | 5.5 | 13x | 0, 0 |
| XamContentGetLocalizedString | 70-72 | 10.9 | 6.4-6.6x | 0, 0 |
| XamContentGetLocalizedDeviceData (disc) | 70-72 | 13.2-13.4 | 5.3x | 0, 0 |
| XamContentGetAttributesInternal | 69-72 | 22,269-23,100 | (a host stat) | 304, 3 |
| XamContentGetLocalizedDeviceData (drive) | 69-74 | 18,156-18,420 | (the drive's space) | 0, 0 |
| XamContentResolveInternal (mounted again) | 68-69 | 571-585 | 0.1x | 528, 7 |

The two calls that ask the host are at its level. Against the host's own call (`sys_step12_bench`):
- the folder's attributes: 22.7-24.2 us against `GetFileAttributesEx`'s 22.3-25.8 us (0.98-1.06x);
- the drive data: 18.4-21.1 us against `std::filesystem::space`'s 18.2-20.4 us (0.97-1.03x).

The original is the reference runtime's stub, which asks nothing.

- **The same mount again** (generic, `FileSystem::AddMount`): a device mounted again on the same folder with the same
  rights keeps its entry, with nothing made. `XamContentResolveInternal` on a package already mounted went from 9
  allocations (760 B) to 7 (528 B), its time unchanged (557-583 ns `before/`, 571-585 after). Kept for the memory.
  The rest of its time and allocations is the package's host path (`std::filesystem::path`, its wide conversion),
  for a call a title makes once per package.

The answers:
- `XamContentGetAttributesInternal`: the package folder's attributes (0, as Xenia), its creation, last access and last
  write times (FILETIMEs stored as the file calls store them) and size 0. A missing package gives
  ERROR_FUNCTION_FAILED, the overlapped untouched.
- `XamContentGetDeviceVolumePath`: "hdd0\" for the hard drive, truncated to the size and terminated;
  ERROR_FUNCTION_FAILED for another device.
- `XamContentGetLocalizedDeviceData`: `XamContentGetDeviceData`'s 0x50 bytes, named by the device's type: "Hard
  Drive", and "Optical Drive" for the disc. ERROR_DEVICE_NOT_CONNECTED for another device.
- `XamContentGetLocalizedString`: the display name and description in the console's language.
  - Languages 10-12 come from metadata of version 2 or later only, else empty.
  - Language 0 takes the display name from slot 1 and the description from the English slot, as Xenia does.
  - Each string is truncated to its size in characters and terminated. ERROR_INVALID_PARAMETER without metadata.
- `XamContentResolveInternal`, on the hard drive: "\Device\Content\<type>\<file name>", mounted on the package
  folder, which is made when asked.
  - With a root name, "<name>:" linked to that path. ERROR_ALREADY_EXISTS when the name is taken, the path still
    given.
  - On the disc: Xenia's "D:\content\<XUID for saved games>\<title id>\<type>\<file name>" (no package is there).
  - The path is truncated to the size. ERROR_INVALID_PARAMETER for another device, a root name off the hard drive or
    a name that names no folder of its own.
  - Unlike `XamContentResolve`, which answers ERROR_FILE_NOT_FOUND for a missing package and
    ERROR_INSUFFICIENT_BUFFER for a short buffer, this follows Xenia's internal form.
- `XamReadTile` / `XamReadTileEx`: the profile has no tiles. The size is 0 and the answer ERROR_FILE_NOT_FOUND, or
  ERROR_INSUFFICIENT_BUFFER without a buffer; through an overlapped, the extended error is its HRESULT.
  ERROR_INVALID_PARAMETER when neither the user index nor the item id (as a XUID) names the local profile, or without
  a size. In `XamReadTileEx` the user index is the fourth argument and the overlapped the ninth, on the stack.
- The three launch calls: ERROR_FILE_NOT_FOUND, as `XamContentLaunchImage`. No image is loaded besides the title's.

Checked by `sys_adapter_tests` (`ContentAdapters`): each adapter is called with its arguments in the registers a title
uses (`XamReadTileEx`'s ninth on the stack), with r3, the guest bytes and the overlapped checked, on a package made by
`XamContentCreate`. It covers:
- the folder made by `XamContentResolveInternal` and the path resolving through the mount and the root name;
- the truncations;
- languages 3 and 10 with metadata of versions 1 and 2.
