# Step 12, fifth batch: 16 guide-screen and Kinect (NUI) calls

The calls Xenia implements and the reference runtime stubs (docs/SYS_STUBS.md, "nui" and "ui"):
`XamNuiHudGetEngagedTrackingID`, `XamNuiHudGetInitializeFlags`, `XamNuiHudGetVersions`, `XamNuiHudIsEnabled`,
`XamNuiHudSetEngagedTrackingID`, `XamNuiIdentityGetSessionId`, `XamNuiIsChatMicEnabled`, `XamNuiIsDeviceReady`,
`XamSetDashContext`, `XamShowCreateProfileUI`, `XamShowCreateProfileUIEx`, `XamShowEditProfileUI`,
`XamShowForcedNameChangeUI`, `XamShowGamerCardUI`, `XamShowMultiplayerUpgradeUI`, `XamShowNuiHardwareRequiredUI`.
Measured on 2026-11-18 with `sys_abi_bench 9` (the original through the PowerPC ABI) and `sys_step12_bench 9`, three
times each. Raw output: `sys_abi_bench_run1-3.txt`, `sys_step12_bench_run1-3.txt` (every step 12 call is in them).

Reused, not recoded: the guide that opens and closes at once (`Xam::ShowLiveUI`: XN_SYS_UI 1 then 0, as the other
Live screens), the sign-in state of the one local profile, the notification queue, the troubleshooter's answer.

Against the original (the reference's stub, 67-78 ns a call; three runs):

| calls | Any360 ns | speed |
|---|---|---|
| the Kinect answers (not ready, HUD off, no flags, no engaged player, mic off) | 1.6-2.0 | 34-49x |
| XamNuiHudGetVersions, XamNuiHudSetEngagedTrackingID | 2.8-3.2 | 21-26x |
| XamNuiIdentityGetSessionId | 3.4-4.3 | 16-21x |
| XamSetDashContext | 3.7-3.9 | 18-20x |
| the guide screens that open (create profile, edit profile, gamer card, forced name change), no listener | 4.3-4.9 | 14-17x |
| XamShowMultiplayerUpgradeUI, XamShowNuiHardwareRequiredUI | 2.4-3.0 | 23-29x |

- **With a listener** (a title has one): a guide screen plus the two `XNotifyGetNext` that read its open and close
  back, 33-34 ns in all (`sys_step12_bench`).
- **Against the host**: nothing on the host does this work (the console's guide and Kinect); no host column.
- **Host memory**: 0 B and 0 allocations a call for every row, the listener's queue included (it keeps its
  capacity), over 11.7 M (`sys_abi_bench`) and 110 M (`sys_step12_bench`) timed calls a run.

The answers (Xenia's xam_ui.cc and xam_nui.cc, with no Kinect attached):

- Kinect: not ready, the HUD not enabled, initialize flags 0, versions 0, no engaged player (setting one fails with
  E_FAIL, clearing succeeds, as Xenia without NUI data), the chat microphone off. The identity session id is a random
  non-zero 64-bit value made at the first call, as the console's; Xenia uses a constant (0xDEADF00DDEADF00D) and notes
  the console's random one.
- `XamSetDashContext` stores the context and notifies XN_SYS_DASHCONTEXTCHANGED (0x8000000C) when it changes.
- Profiles: one local profile and no way to make another, so the create-profile screens open and close (Xenia
  headless answers the same); edit profile and gamer card refuse a user that is not signed in (ERROR_ACCESS_DENIED);
  the forced name change, a marketplace screen, refuses a user past 3 (ERROR_INVALID_PARAMETER) or not signed in
  (ERROR_NO_SUCH_USER); the multiplayer (Gold) upgrade fails (ERROR_FUNCTION_FAILED, as Xenia); the Kinect-required
  screen takes only 0 (else ERROR_INVALID_PARAMETER) and closes at once.

Checked by `sys_object_tests` (`GuideNuiStep12Tests`): every return value, the guide's notifications seen by a system
listener (opened and closed, or nothing for a refused screen), the dashboard context notified once per change, the
versions written and null pointers skipped, a stable non-zero session id.
