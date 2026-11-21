# Step 12, the calls Xenia stubs too: the 10 information calls

`XamGetCountry`, `XamSetCountry`, `XamGetDvrStorage`, `XamSetDvrStorage`, `XamGetLanguageTypefacePatch`,
`XamGetLiveHiveValueA`, `XamGetLiveHiveValueW`, `XamGetStagingMode`, `XamIsNatalPlaybackEnabled`,
`XamIsNuiAutomationEnabled`, answered as Xenia's (xam_locale.cc, xam_info.cc, xam_nui.cc, xam_state.cc). Measured on
2026-11-21 (AMD Ryzen 7 7800X3D, cores 0-7, the machine otherwise quiet) with `sys_abi_bench 9`, three times
(`sys_abi_bench_run1-3.txt`); every row's result is checked after its timing. No host call does this work.

Reused, not recoded:
- the console settings (`Console().country`, which `ExGetXConfigSetting` and the locale calls read);
- the kernel heap laid out at first use, as the fallback locale names are;
- the overlapped completion (`CompleteEx`).

| call | original ns | Any360 ns | speed | B, allocs a call |
|---|---|---|---|---|
| XamSetDvrStorage, XamIsNatalPlaybackEnabled, XamIsNuiAutomationEnabled, XamGetStagingMode | 65-68 | 1.6-2.4 | 28-42x | 0, 0 |
| XamGetDvrStorage | 67-69 | 2.0-2.4 | 29-34x | 0, 0 |
| XamGetCountry, XamSetCountry | 67-68 | 2.4-2.6 | 26-28x | 0, 0 |
| XamGetLanguageTypefacePatch | 67-68 | 4.0-4.2 | 16-17x | 0, 0 |
| XamGetLiveHiveValueA, XamGetLiveHiveValueW | 67-69 | 4.1-4.3 | 16-17x | 0, 0 |

The answers:
- **The country**: the console's, set for the run by `XamSetCountry` (in memory, as the other console settings are).
- **DVR storage**: none set aside (0 and 0; the drive's unused space left untouched, as Xenia leaves it). Setting it is
  accepted.
- **The font patch**: Xenia's for the language, "file://media:/Xenon<SC|C|JK>Latin.xttp2" for Simplified Chinese (17),
  Traditional Chinese (8) and any other language. It is big-endian UTF-16, the three names laid out once in the
  kernel heap (0 if the heap is full).
- **The Live hive**: no Xbox Live to sign in to, X_ONLINE_E_LOGON_NOT_LOGGED_ON (0x80151802). E_INVALIDARG without a
  name, a value buffer or its size. Through an overlapped, Xenia's immediate completion: that result as both result
  and extended error, the length ~0. Xenia's path for the dashboard itself (it asks `XamQueryLiveHive`) does not
  apply to a title.
- **Staging mode** 0 (a retail console); no Kinect playback or automation (0).

Checked by `sys_adapter_tests` (`InfoAdapters`): each adapter called with its arguments in the registers a title uses,
r3 and the guest bytes checked, the country read back through the console settings, the three font patch strings
compared character by character, both Live hive forms with and without an overlapped.
