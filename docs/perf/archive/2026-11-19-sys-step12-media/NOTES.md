# Step 12, the calls Xenia stubs too: media and the dashboard's services (13)

`EnumerateMediaObjects` (four exports), `XamMediaVerificationCreate`, `Close`, `Inject`, `Verify`, `FailedBlocks`,
`XampWebInstrumentationSetProfileCounts`, `XampXAuthStartup`, `XampXAuthShutdown`, `XampXAuthGetTitleBuffer`, answered
as Xenia's stubs (xam_enum.cc, xam_media.cc, xam_info.cc, xam_net.cc). Measured on 2026-11-19 (AMD Ryzen 7 7800X3D,
cores 0-7) with `sys_abi_bench 9`, three times (`sys_abi_bench_run1-3.txt`); every row's result is checked after its
timing. No host call does this work (a sys_step12_bench row would time the same constants).

| call | original ns | Any360 ns | speed | B, allocs a call |
|---|---|---|---|---|
| EnumerateMediaObjects (the four), XamMediaVerificationCreate / Close / Inject, XampWebInstrumentationSetProfileCounts, XampXAuthGetTitleBuffer | 67-70 | 1.4-2.0 | 33-50x | 0, 0 |
| XampXAuthShutdown | 67 | 1.8-2.2 | 30-37x | 0, 0 |
| XamMediaVerificationVerify (no callback) | 66-68 | 2.4-2.5 | 27-28x | 0, 0 |
| XampXAuthStartup | 68-69 | 3.6-4.1 | 17-19x | 0, 0 |
| XamMediaVerificationFailedBlocks | 69 | 3.9-4.1 | 17-18x | 0, 0 |

The answers: no media library (`EnumerateMediaObjects*` E_NOTIMPL); the disc's verification succeeds, a callback given
to `XamMediaVerificationVerify` queued as a user APC on the calling thread (its low bit cleared, no arguments; the
scheduler's APC queue), `XamMediaVerificationFailedBlocks` reports nothing failed, nothing checked and the check
complete for a structure of 0x18 bytes (ERROR_NOT_ENOUGH_MEMORY for another size); the XAuth service is not there
(`XampXAuthStartup` 0x80158401 for settings whose size field is not 8 or none, else 0x80158406; the count 0, no title
buffer). The row with a callback is not timed (each call leaves an APC queued); `sys_adapter_tests` checks it.

Checked by `sys_adapter_tests` (`MediaAdapters`): each adapter with its arguments in the registers a title uses, r3
and the guest bytes, the APC queued only with a callback, both structure sizes, the XAuth settings' sizes.
