# Step 12, the calls Xenia stubs too: the last 7 (SMC, tray, microphone, cancel, rip)

`HalSendSMCMessage`, `HalOpenCloseODDTray`, `MicDeviceRequest`, `NtCancelIoFile`, `RtlRip`,
`XAutomationpUnbindController`, `XamVoiceGetMicArrayStatus`, answered as Xenia's (smc.cc, xboxkrnl_hal.cc,
xboxkrnl_misc.cc, xboxkrnl_io.cc, xboxkrnl_rtl.cc, xam_input.cc, xam_voice.cc). With them, none of the 114 calls the
reference runtime and Xenia both stub is left: every one is answered natively.

Measured on 2026-11-21 (AMD Ryzen 7 7800X3D, cores 0-7) with `sys_abi_bench 9`, three times
(`sys_abi_bench_run1-3.txt`); every row's result is checked after its timing. No host call does this work.

Reused, not recoded:
- the notification listeners (`Xam::Notify`, for the tray's change);
- the AV pack the layer reports (`XGetAVPack`: VGA);
- the guest calls on the calling thread (`Threads().Dispatch`, as LDI's allocator), for the microphone's completion
  routine;
- the open files (`FileSystem::Find`);
- the debug sink `DbgPrint` and `OutputDebugString` write to.

| call | original ns | Any360 ns | speed | B, allocs a call |
|---|---|---|---|---|
| XAutomationpUnbindController, XamVoiceGetMicArrayStatus | 70-77 | 1.8-2.1 | 34-42x | 0, 0 |
| MicDeviceRequest (a status request) | 71-74 | 2.9-5.7 | 12-26x | 0, 0 |
| RtlRip (no debug sink) | 71-75 | 3.7-4.4 | 17-20x | 0, 0 |
| HalOpenCloseODDTray (its notification, no listener) | 73-79 | 3.9-5.8 | 13-20x | 0, 0 |
| HalSendSMCMessage (temperatures) | 75-82 | 11.4-11.7 | 6.5-7.2x | 0, 0 |
| NtCancelIoFile (a handle that is not a file) | 70-79 | 13.7-15.7 | 4.4-5.7x | 0, 0 |

`HalSendSMCMessage` writes the response's nine bytes one by one into guest memory. `NtCancelIoFile` looks the handle up
in the object table and the open files under their lock.

The answers:
- **The SMC** (`HalSendSMCMessage`, new state in `Xam`). A message and its response are 16 bytes: the command, then
  its data.
  - Queries answered with the command byte and their fields, as Xenia's model does:
    - temperatures (CPU, GPU and EDRAM 69.6 C, motherboard 69.9 C, as Xenia stores them);
    - the tray;
    - the AV pack (VGA, 0x5B, matching `XGetAVPack`);
    - the SMC version (65, 2, 1);
    - the IR address (the Xbox 360 remote);
    - the tilt (vertical).
  - Settings kept: the tray, the CPU and GPU fan speeds, the power LED, the LEDs. `SET_IR_ADDRESS` answers the current
    address without changing it, as Xenia.
  - Any other command, or no message: nothing.
  - **The tray starts closed**, since a title runs from its disc. Xenia starts it open. A change notifies
    XN_SYS_TRAYSTATECHANGED with the new state; `HalOpenCloseODDTray` opens or closes it.
- **`MicDeviceRequest`**: no microphone.
  - STATUS_INVALID_PARAMETER without a device or for a user past 4; STATUS_INVALID_DEVICE_REQUEST for a request past
    0x0B.
  - The state is set to not connected. A status request succeeds.
  - The other requests call the completion routine, when one is given, with the device and STATUS_UNSUCCESSFUL on the
    calling thread, and answer STATUS_PENDING, as Xenia. The capabilities request also fills features 0x100, format 1
    and colour 0.
- **`NtCancelIoFile`**: a file's transfers all complete before their call returns, so there is nothing to cancel:
  success for an open file, STATUS_INVALID_HANDLE otherwise.
- **`RtlRip`**: the title's assertion goes to the debug sink as "RtlRip: <message> <detail>", and the title goes on, as
  Xenia lets it.
- **`XAutomationpUnbindController`**: 1 for users 0-3, 0 past them. **`XamVoiceGetMicArrayStatus`**: 0, no Kinect
  microphone array.

Checked by `sys_adapter_tests` (`LastAdapters`):
- each adapter is called with its arguments in the registers a title uses, r3 and the guest bytes checked;
- every SMC query's bytes and untouched rest, the tray through both calls with the notification read back, the fans
  and LEDs kept;
- the microphone's checks, state and capabilities, and its completion routine called through the scheduler's
  dispatcher with the device and STATUS_UNSUCCESSFUL, and not called without one;
- `NtCancelIoFile` on a file opened by `NtOpenFile`, then on its handle once closed;
- `RtlRip`'s text through the debug sink.
