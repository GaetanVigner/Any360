# Step 12, the calls Xenia stubs too: the 22 avatar calls

The first group of the 114 calls the reference runtime stubs and Xenia stubs too (docs/SYS_STUBS.md, "retail-stub"):
`XamAvatarBeginEnumAssets`, `EndEnumAssets`, `EnumAssets`, `GenerateMipMaps`, `GetAssetBinary`, `GetAssetIcon`,
`GetAssets`, `GetAssetsResultSize`, `GetInstalledAssetPackageDescription`, `GetInstrumentation`,
`GetManifestLocalUser`, `GetManifestsByXuid`, `GetMetadataRandom`, `GetMetadataSignedOutProfile`,
`GetMetadataSignedOutProfileCount`, `LoadAnimation`, `ManifestGetBodyType`, `ReinstallAwardedAsset`,
`SetCustomAsset`, `SetManifest`, `SetMocks`, `WearNow`. Measured on 2026-11-19 (AMD Ryzen 7 7800X3D, cores 0-7,
another session's test suite on cores 8-15) with `sys_abi_bench 9` and `sys_step12_bench 9`, three times each; the
first measure, before the two optimizations below, in `before/`. Every row's result is checked after its timing.

Reused, not recoded: the overlapped completion (`Finish`), the profile settings' store (the write of
`XamUserWriteProfileSettings` made a function, `PutSetting`, which the manifest uses: a binary setting is also written
to the profile folder, as before).

Against the original (the reference's stub, 66-72 ns a call; no row allocates once warm):

| call | original ns | Any360 ns | speed |
|---|---|---|---|
| XamAvatarEnumAssets, ManifestGetBodyType, ReinstallAwardedAsset, SetCustomAsset, SetMocks | 67-70 | 1.6 | 42-44x |
| XamAvatarGetInstrumentation | 67-68 | 3.4-3.5 | 19-20x |
| the eleven asynchronous ones (Begin/EndEnumAssets, GenerateMipMaps, GetAssetBinary, GetAssetIcon, GetManifestsByXuid, GetMetadataRandom, GetMetadataSignedOutProfile(Count), LoadAnimation, WearNow) | 66-71 | 3.6-4.5 | 15-19x |
| XamAvatarGetAssets, GetAssetsResultSize | 67-70 | 3.8-4.0 | 17-18x |
| XamAvatarGetInstalledAssetPackageDescription | 70-72 | 13.8-15.2 | 4.6-5.2x |
| XamAvatarGetManifestLocalUser | 66 | 14.1-15.2 | 4.3-4.7x |
| XamAvatarSetManifest (no profile folder: in memory) | 67-68 | 16.4-19.0 | 3.6-4.1x |

Without the ABI (`sys_step12_bench`): SetManifest 16.2-16.6 ns, GetManifestLocalUser 12.9-13.8 ns, the asset's
package 12.9-13.2 ns. No host call does this work.

- **GetInstalledAssetPackageDescription**: the file name formatted by `snprintf` first, 104-116 ns (0.6-0.7x the
  original, `before/`); the 32 hexadecimal digits written from the asset id's bytes: 13-15 ns. Kept.
- **SetManifest**: a new 1016-byte setting each call first (one allocation, 42-48 ns); the kept setting rewritten in
  place: 16-19 ns and no allocation. Kept.

The answers (Xenia's xam_avatar.cc): avatars are not initialized (`XamAvatarInitialize` fails, as Xenia's does); the
asynchronous calls succeed and complete their overlapped (ERROR_IO_PENDING); `XamAvatarGetAssets` through an
overlapped completes with ERROR_FUNCTION_FAILED and E_FAIL; `XamAvatarEnumAssets` is E_NO_MORE_FILES; the result
sizes 0; the body type male (1); the instrumentation value 0, result 1; `XamAvatarWearNow` from the avatar editor
completes nothing. The manifest is the profile's `XPROFILE_GAMERCARD_AVATAR_INFO_1` (0x63E80044): set by
`XamAvatarSetManifest` (user 0; others E_NO_SUCH_USER), read back by `XamAvatarGetManifestLocalUser` (a user past 3
or no buffer E_INVALIDARG, none kept E_FAIL; without an overlapped the extended error is returned) and by
`XamUserReadProfileSettings`. An asset's package: avatar item (0x9000), XUID 0, the asset's title id, the file name
of its 16 bytes in hexadecimal; Xenia reads the X_ASSET_ID without swapping its fields, Any360 big-endian.

Checked by `sys_adapter_tests` (new, in the suite): each of the 22 adapters called with its arguments in the
registers a title uses, r3, the overlapped and the guest bytes checked; with the manifest read back through
`XamUserReadProfileSettings`. The same executable checks the adapters of step 12's eighth to tenth batches, which
`sys_object_tests` checked only behind them (LDI end to end, ObCreateObject through the scheduler's dispatcher).
