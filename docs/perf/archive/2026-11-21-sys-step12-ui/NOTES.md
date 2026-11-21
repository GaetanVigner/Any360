# Step 12, the calls Xenia stubs too: the 8 guide screens

`XamShowGoldUpgradeUI`, `XamShowMarketplaceUIEx`, `XamShowNuiDeviceSelectorUI`, `XamShowNuiDirtyDiscErrorUI`,
`XamShowNuiGuideUI`, `XamShowNuiMessageBoxUI`, `XamShowNuiSigninUI`, `XamShowSigninUIEx`, answered as Xenia's
(xam_ui.cc, nui.cc). Measured on 2026-11-21 (AMD Ryzen 7 7800X3D, cores 0-7) with `sys_abi_bench 9`, three times
(`sys_abi_bench_run1-3.txt`); every row's result is checked after its timing. The original's rows ran from 68 to
116 ns, as another session was working on the machine. No host call does this work.

Reused, not recoded:
- the guide opening and closing at once (`ShowLiveUI`: XN_SYS_UI 1, then 0), the sign-in screen's answer
  (`ShowSigninUI`);
- the overlapped completion (`CompleteEx`).

| call | original ns | Any360 ns | speed | B, allocs a call |
|---|---|---|---|---|
| XamShowGoldUpgradeUI and the five Kinect forms | 68-116 | 2.0-2.3 | 32-55x | 0, 0 |
| XamShowSigninUIEx (no overlapped) | 113-114 | 5.7-8.9 | 13-20x | 0, 0 |
| XamShowMarketplaceUIEx | 68-115 | 8.5-8.8 | 8-14x | 0, 0 |

The two guide screens cost their two notifications (no listener in the bench: they go nowhere).

The answers:
- **`XamShowSigninUIEx`**: the profile is already signed in, so the guide opens and closes at once, as
  `XamShowSigninUI`. Through an overlapped it uses Xenia's immediate completion (the result as result and extended
  error, the length ~0 on failure) and returns ERROR_IO_PENDING.
- **`XamShowMarketplaceUIEx`**: the marketplace needs Xbox Live, so the guide opens and closes at once, as
  `XamShowMarketplaceUI`. It answers ERROR_INVALID_PARAMETER for a user past 3 or an entry point past the last (24),
  and ERROR_NO_SUCH_USER for a user not signed in, as Xenia; a refused call opens nothing.
- **`XamShowGoldUpgradeUI`**: ERROR_FUNCTION_FAILED, there is no Gold membership to buy offline.
- **The five Kinect forms**: Xenia checks the Kinect HUD first, and without a Kinect it answers ERROR_ACCESS_DENIED
  with nothing shown.

Checked by `sys_adapter_tests` (`UiAdapters`): each adapter called with its arguments in the registers a title uses,
r3 checked; the guide's XN_SYS_UI 1 and 0 read back through a notification listener for the two screens shown, none
for the refused calls; the overlapped of `XamShowSigninUIEx`.
