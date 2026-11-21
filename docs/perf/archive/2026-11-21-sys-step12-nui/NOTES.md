# Step 12, the calls Xenia stubs too: the 10 Kinect (NUI) calls

`XamNuiCameraElevationGetAngle`, `XamNuiCameraGetTiltControllerType`, `XamNuiCameraSetFlags`,
`XamNuiCameraTiltGetStatus`, `XamNuiCameraTiltSetCallback`, `XamNuiGetDepthCalibration`, `XamNuiIdentityAbort`,
`XamNuiIdentityEnrollForSignIn`, `XamNuiPlayerEngagementUpdate`, `XamNuiSkeletonGetBestSkeletonIndex`, answered as
Xenia's (xam_nui.cc) without a Kinect: no tilt controller, the HUD not enabled.

Measured on 2026-11-21 (AMD Ryzen 7 7800X3D, cores 0-7) with `sys_abi_bench 9`, three times
(`sys_abi_bench_run1-3.txt`); every row's result is checked after its timing. The original's rows ran from 67 to
115 ns this time: another session was working on the machine. Any360's side stayed at 1.6-4.8 ns. No host call does
this work.

| call | original ns | Any360 ns | speed | B, allocs a call |
|---|---|---|---|---|
| ElevationGetAngle, GetTiltControllerType, SetFlags, TiltGetStatus, IdentityAbort, IdentityEnrollForSignIn, SkeletonGetBestSkeletonIndex | 68-115 | 1.6-2.3 | 31-54x | 0, 0 |
| XamNuiGetDepthCalibration (0x30 bytes cleared) | 90-114 | 2.6-2.8 | 33-43x | 0, 0 |
| XamNuiPlayerEngagementUpdate (0x1C bytes cleared) | 82-115 | 2.7-2.8 | 30-42x | 0, 0 |
| XamNuiCameraTiltSetCallback | 67-76 | 2.6-4.8 | 14-26x | 0, 0 |

The answers:
- **No tilt controller** (type 0). The motor's status and the elevation angle answer E_FAIL with nothing written;
  setting the flags answers E_DEVICE_NOT_CONNECTED. The tilt callback is kept, cleared at reset, and never called.
- **No depth calibration stored**: the 0x30 bytes are cleared, then STATUS_NO_SUCH_FILE with the NT facility bit
  (0xD000000F).
- **Identity abort and enrollment**: E_FAIL, the HUD not being enabled.
- **The engagement update**: its 0x1C bytes are cleared, with no result.
- **The best skeleton**: none, a 64-bit -1.

Checked by `sys_adapter_tests` (`NuiAdapters`):
- each adapter is called with its arguments in the registers a title uses, r3 and the guest bytes checked;
- the cleared structures are cleared to their size and no further;
- the angle's outputs stay untouched;
- r3 is left as it was by the engagement update, which has no result.
