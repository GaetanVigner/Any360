# Step 12, the calls Xenia stubs too: the 9 other calls (settings, LDI reset, dashboard odds)

`ExSetXConfigSetting`, `ExReadModifyWriteXConfigSettingUlong`, `LDIResetDecompression`, `XamDoesOmniNeedConfiguration`,
`XamFirstRunExperienceShouldRun`, `XamLogLocalizationEtx`, `XamLookupCommonStringByIndex`, `XamQueryLiveHiveA`,
`XamXStudioRequest`, answered as Xenia's (xboxkrnl_xconfig.cc, xboxkrnl_ldi.cc, xam_info.cc). Measured on 2026-11-21
(AMD Ryzen 7 7800X3D, cores 0-7) with `sys_abi_bench 9`, three times (`sys_abi_bench_run1-3.txt`); every row's result
is checked after its timing. The original's rows ran from 67 to 119 ns, as another session was working on the machine;
Any360's side stayed within a few ns. No host call does this work.

Reused, not recoded:
- the console settings `ExGetXConfigSetting` reads (`ConsoleSettings`, now holding every setting it answers);
- the LZX decoder's own reset (`LzxDecoder::Reset`, under the contexts' lock);
- the overlapped completion (`CompleteEx`).

| call | original ns | Any360 ns | speed | B, allocs a call |
|---|---|---|---|---|
| XamDoesOmniNeedConfiguration, XamFirstRunExperienceShouldRun, XamLogLocalizationEtx, XamLookupCommonStringByIndex, XamXStudioRequest | 70-116 | 1.8-2.8 | 28-51x | 0, 0 |
| ExSetXConfigSetting | 67-113 | 3.4-5.6 | 18-20x | 0, 0 |
| ExReadModifyWriteXConfigSettingUlong | 69-85 | 3.4-5.6 | 12-25x | 0, 0 |
| XamQueryLiveHiveA | 100-113 | 7.3-8.3 | 12-16x | 0, 0 |
| LDIResetDecompression | 70-119 | 20-34 | 2.4-3.5x | 0, 0 |

`LDIResetDecompression` does real work: it clears the main and length trees' code lengths, about 900 bytes.

The answers:
- **Settings**:
  - `ExSetXConfigSetting` stores a setting `ExGetXConfigSetting` reads, for the run: a word or a byte, at the
    setting's own size, big-endian, whatever size is given.
  - `ExReadModifyWriteXConfigSettingUlong` sets value = (value & kept) | set; a byte setting keeps its low byte.
  - Both answer STATUS_SUCCESS for a setting the layer does not keep, with nothing stored, as Xenia;
    `ExSetXConfigSetting` answers STATUS_INVALID_PARAMETER_3 without a buffer.
  - The country set this way is `XamGetCountry`'s.
  - The settings are now `ConsoleSettings` fields with their defaults: AV region, time zone, video, audio, retail and
    parental control flags. `ExGetXConfigSetting` reads them there, with the same answers as before.
- **`LDIResetDecompression`**: the context's decoder goes back to a stream's start (window position, repeated offsets,
  trees, Intel E8 state), the window kept. MDI_ERROR_BAD_PARAMETERS for an unknown handle.
- **No wireless network to set up**, the first run done, no common string table (0), no xstudio.xex (E_FAIL).
- **Localization errors**: the two Xenia knows (0x80300034, 0x80300035) logged with success, any other E_NOTIMPL.
- **`XamQueryLiveHiveA`**: no Live hive offline. Without an overlapped, the extended error is the answer:
  X_ONLINE_E_LOGON_NOT_LOGGED_ON, or E_INVALIDARG without a name, a value buffer or its size. With an overlapped, it
  completes with success and that extended error and returns E_PENDING (0x8000000A), as Xenia does. This differs from
  `XamGetLiveHiveValueA/W`, whose overlapped carries the error as its result.

Checked by `sys_adapter_tests` (`MiscAdapters`; the LDI reset beside the earlier LDI checks):
- a setting set and read back through `ExGetXConfigSetting` (word, byte, a smaller size given);
- the country through `XamGetCountry`;
- the read-modify-write on a word and a byte setting;
- settings the layer does not keep, and no buffer;
- the same LZX chunk decoding again after a reset, and the reset of a destroyed context failing;
- the Live hive with and without an overlapped.

The settings are put back after the test.
