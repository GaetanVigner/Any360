# Step 12, fourth batch: 11 kernel and XAM calls

The calls Xenia implements and the reference runtime stubs (docs/SYS_STUBS.md, "rtl", "threading", "hal", "locale"):
`RtlGetLastError`, `RtlSetLastNTError`, `RtlSleep`, `RtlRandom`, `RtlDowncaseUnicodeChar`, `RtlGetStackLimits`,
`RtlImageDirectoryEntryToData`, `KeQueryInterruptTime`, `KeSetPriorityThread`, `HalGetCurrentAVPack`,
`XamGetLocaleDateFormat`. Measured on 2026-11-18 with `sys_abi_bench 9` (the original through the PowerPC ABI) and
`sys_step12_bench 9` (beside the host's own code), three times each on cores 0-7. Raw output:
`sys_abi_bench_run1-3.txt`, `sys_step12_bench_run1-3.txt` (every step 12 call is in them).

Reused, not recoded: the last error and its NTSTATUS mapping (`Win32LastError`, `SetWin32LastNtError`), `SleepEx`'s
delay for `RtlSleep`, `RtlImageNtHeader` for the PE header, the PCR's stack fields, the KTHREAD's priority byte,
`XGetAVPack`, the clock of `KeTimeStampBundle` for the interrupt time.

Against the original (the reference's stub, 67-72 ns a call; Any360 / original as speed, three runs):

| call | Any360 ns | speed |
|---|---|---|
| RtlGetLastError | 3.1-3.3 | 21-22x |
| RtlSetLastNTError | 7.0-7.2 | 9.6-9.9x |
| RtlRandom | 6.0-6.2 | 11x |
| RtlDowncaseUnicodeChar | 2.8-3.1 | 23-25x |
| RtlGetStackLimits | 2.7-2.9 | 24-25x |
| RtlImageDirectoryEntryToData (a file, through the sections) | 7.6-11.8 | 6-9x |
| KeQueryInterruptTime | 16.2-16.8 | 4.1-4.2x |
| KeSetPriorityThread | 9.9-10.3 | 6.7-7.1x |
| HalGetCurrentAVPack | 2.7-2.8 | 24-26x |
| XamGetLocaleDateFormat | 2.9-3.0 | 23-24x |

`RtlSleep` is left out of the timing as `Sleep` is: it blocks; its cost is `KeDelayExecutionThread`'s.

Against what the host offers for the same work:

- `RtlDowncaseUnicodeChar` 1.3 ns, the C library's `towlower` 2.4 ns: 1.8-1.9x (and `towlower`'s answer depends on
  the host's locale, which is why Xenia's can differ from machine to machine; Any360 mirrors its own
  `RtlUpcaseUnicodeChar`).
- `KeQueryInterruptTime` 16.2-16.8 ns, the host's performance counter alone 17.1-17.8 ns: 1.02-1.08x. It used to go
  through `steady_clock` and a nanosecond division (0.89x); it now reads Windows' counter, which runs at 10 MHz:
  100-ns units already.
- `GetTickCount` 17-18 ns (now from the same counter) against the host's `GetTickCount64` 1.0 ns: 0.06x, but not the
  same quantity: `GetTickCount64` reads a value the system updates every 10-16 ms, where the console's tick moves
  every millisecond; a title timing its frames with it would see 16 ms steps. A cached tick from Any360's clock
  thread would need the host timer at 1 ms, which is not guaranteed. The performance counter is the host's source
  with the console's precision, and Any360 is at par with it.
- **Host memory**: 0 B and 0 allocations a call for every Any360 row (8.8 M timed calls a run of `sys_abi_bench`).

Where Any360 follows Xenia and where not: `RtlRandom`'s sequence and table, `RtlGetStackLimits`'s alternate stack,
`RtlImageDirectoryEntryToData`'s odd base and one-byte flag, `XamGetLocaleDateFormat`'s table (Xenia's holds 109
entries but reads the first 105, as here) are Xenia's. `KeSetPriorityThread` sets a negative priority as 0 and leaves
a KTHREAD the scheduler does not run unchanged, as Xenia; it moves no host thread priority (any360::sys does not map
guest priorities to host ones yet, `KeSetBasePriorityThread` neither).

Checked by `sys_object_tests` (`KernelStep12Tests`, and `SchedulerTests` for the priority on a running thread):
every value and byte against Xenia's answers, RtlRandom's sequence modelled step by step, a PE32 image with entries in
the headers, in a section and in none.
