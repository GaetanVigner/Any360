# Step 12, third batch: xam.xex's 19 information calls

The information calls Xenia implements and the reference runtime stubs (docs/SYS_STUBS.md, "info"): `XGetAudioFlags`,
`XGetOverlappedExtendedError`, `XamGetActiveDashAppInfo`, `XamSetActiveDashAppInfo`, `XamGetCachedTitleName`,
`XamGetDashBackstackData`, `XamGetDashContext`, `XamGetLanguage`, `XamGetLanguageLocaleFallbackString`,
`XamGetLanguageTypeface`, `XamGetOnlineCountryFeatures`, `XamGetOnlineLanguageAndCountry`, `XamIsChildAccountSignedIn`,
`XamIsCurrentTitleDash`, `XamIsIptvEnabled`, `XamIsNuiUIActive`, `XamIsSystemExperienceTitleId`, `XamIsSystemTitleId`,
`XamIsXbox1TitleId`. Measured on 2026-11-18 with `sys_step12_bench 9` three times (fastest of 9 runs each,
RelWithDebInfo, clang, the owner's desktop). Raw output: [run 1](sys_step12_bench_run1.txt),
[run 2](sys_step12_bench_run2.txt), [run 3](sys_step12_bench_run3.txt) (the rows of the first two batches are in them
too).

- **Original**: none to time. The reference stubs every one (it returns at once and does nothing), and no known title
  calls them yet.
- **Against the host**: nothing on the host does this work (they are console facts and XAM's own state), so no host
  column. Before writing each, the existing any360::sys code was checked and reused: the console's language and
  country (`Console()`, as `ExGetXConfigSetting` reports them), the game region (`XGetGameRegion`), the thread's last
  error (`Win32LastError`), the kernel heap for the fallback names (`Objects().AllocateObject`, as `XamGetOnlineSchema`),
  ERROR_IO_PENDING from xam.h.
- **Time a call** (three runs): 1.0-3.1 ns for the constants, predicates and the state copies (`XGetAudioFlags`,
  `XamGetLanguage`, the three title-id classes, `XGetOverlappedExtendedError`, the dashboard info get / set, the online
  language and country); 5.5-5.8 ns for `XamGetOnlineCountryFeatures` (a scan of Xenia's 45-country table);
  2.7-2.8 ns for `XamGetLanguageLocaleFallbackString` after its first call; 16.0-16.4 ns for `XamGetLanguageTypeface`
  and 19.9-20.6 ns for `XamGetCachedTitleName` (22 characters), which write big-endian UTF-16 unit by unit.
- **Host memory**: 0 B and 0 allocations a call for every one (91.9 M timed calls over the whole benchmark). The
  fallback names take 200 bytes of the guest's kernel heap once, at the first `XamGetLanguageLocaleFallbackString`.

The answers and where Any360 differs from Xenia:

- The title-id classes, the typefaces, the fallback names, the country features and `XamGetLanguage`'s region rule are
  Xenia's (its static_asserts are among the tests).
- `XGetAudioFlags` is XAM's default (0x10001): Any360 keeps no audio setting, and its AV pack is VGA (Xenia answers 2
  only for AV pack 2).
- `XamGetCachedTitleName` writes `TitleSettings::title_name` (new, empty until the host sets it) for any id but 0. Xenia
  also looks up other titles a profile played; Any360 has one profile and one title. Neither reads the length first.
- `XamGetDashBackstackData` and `XamGetDashContext`: no dashboard ran before the title, so no backstack and context 0.
- `XamIsChildAccountSignedIn` 0, `XamIsIptvEnabled` E_FAIL and `XamIsNuiUIActive` 0, matching any360::sys's existing
  answers: no parental controls, no IPTV, and a system UI that opens and closes at once (`XamIsUIActive`).

Checked by `sys_object_tests` (`XamInfoStep12Tests`): every value and every byte written, null pointers, the 0 id, the
language past the 18, the region fallback, an unlisted country.
