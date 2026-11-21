# Step 12, the calls Xenia stubs too: input, network and notification (9)

`XamInputNonControllerGetRaw`, `XamInputNonControllerGetRawEx`, `XamInputNonControllerSetRaw`,
`XamInputNonControllerSetRawEx`, `NetDll_XHttpStartup`, `NetDll_XHttpOpen`, `XamNetworkStorageHasUserEnabledStorage`,
`XNotifyBroadcast`, `XNotifyQueueUI`, answered as Xenia's (xam_input.cc, xam_net.cc, xam_user.cc, xam_notify.cc,
xam_ui.cc). Measured on 2026-11-21 (AMD Ryzen 7 7800X3D, cores 0-7) with `sys_abi_bench 9`, three times
(`sys_abi_bench_run1-3.txt`); every row's result is checked after its timing. No host call does this work.

Reused, not recoded: the thread's Win32 last error (`SetWin32LastError`), the user preferences word
(`XamUserWriteUserPreference`'s), the notification listeners (`Xam::Notify`).

| call | original ns | Any360 ns | speed | B, allocs a call |
|---|---|---|---|---|
| the four non-controller raw calls | 69-71 | 1.8-3.2 | 22-38x | 0, 0 |
| XNotifyQueueUI | 69-70 | 2.0-2.4 | 30-34x | 0, 0 |
| NetDll_XHttpStartup, NetDll_XHttpOpen (the last error set) | 69-70 | 3.1-4.1 | 17-23x | 0, 0 |
| XamNetworkStorageHasUserEnabledStorage | 72-73 | 3.5-3.7 | 20x | 0, 0 |
| XNotifyBroadcast (no listener) | 69-71 | 4.1-4.5 | 15-17x | 0, 0 |

The network storage row answers 1: the bench's `XamUserWriteUserPreference` row before it sets user 0's network
storage preference.

The answers:
- **Non-controller devices** (a portal of power, devices 5 and 6): the device and the buffer are checked as Xenia
  checks them (0x20 bytes at most, a state, length and buffer given), then no such device is plugged in:
  ERROR_DEVICE_NOT_CONNECTED, nothing written. The plain forms are device 5.
- **XHTTP**: startup succeeds (1, last error 0); no session opens (0, last error ERROR_NOT_SUPPORTED).
- **Network storage**: on when the signed-in user's preference bit 0x20 is set, else the privilege decides, which no
  user holds offline (0).
- **`XNotifyBroadcast`** sends the notification to every listener whose areas take it.
- **`XNotifyQueueUI`**: a toast the guide would show; there is no guide to show it on, so success.

Checked by `sys_adapter_tests` (`InputNetNotifyAdapters`):
- each adapter is called with its arguments in the registers a title uses, r3 and the guest bytes checked;
- every check of the raw calls, with their outputs untouched;
- the calling thread's last error after each XHTTP call;
- the storage answer before and after the preference is written;
- a broadcast heard by a listener of its area and not by one of another.
