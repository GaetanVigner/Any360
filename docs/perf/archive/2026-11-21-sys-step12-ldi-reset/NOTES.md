# Step 12: LDIResetDecompression, a lighter LZX reset

The owner asked whether `LDIResetDecompression` could be made faster. It was 20-34 ns through the ABI
(`../2026-11-21-sys-step12-misc/`).

Measured on 2026-11-21 (AMD Ryzen 7 7800X3D, cores 0-7):
- through the ABI with `sys_abi_bench 9`, three times (`sys_abi_bench_run1-3.txt`);
- apart, with `ldi_reset_probe.cpp`, three rounds in `ldi_reset_probe_runs.txt`. The probe is built twice, against the
  previous `ldi.cpp` (HEAD) and the new one, with the same flags.

**Where the time went** (the probe, before):
- the decoder's `Reset` was 8.0-9.7 ns, whatever the window: it cleared the main and length trees' code lengths in
  full, about 1 KB;
- the context's lock and look-up were about 8 ns, the same as for an unknown handle.

**The change:** the decoder keeps how far those lengths were written since its last reset, and `Reset` clears only
that much:
- nothing for a stream of uncompressed blocks only, which reads no tree;
- the main tree's symbols for the window plus the 64-byte run margin, when a verbatim or aligned block read the trees;
- the length tree in full.

The mark is set before the lengths are read, so a corrupt block that wrote some is covered too.

| measure | before | after |
|---|---|---|
| LDIResetDecompression through the ABI (original 68-71 ns) | 20-34 ns (2.4-3.5x) | 11.3-15.7 ns (4.5-6.1x) |
| `LzxDecoder::Reset`, nothing decoded since (probe) | 8.0-9.7 ns | 1.6-1.7 ns |
| `Ldi().ResetDecompression` (probe) | 15.2-16.2 ns | 9.2-9.5 ns (one round 15.1) |
| a reset then a stored chunk decoded (probe) | 38.2-39.9 ns | 31.2-36.3 ns |
| a reset then a compressed chunk decoded, trees read (probe) | 4.87-5.09 us | 4.91-5.15 us (no change) |
| the 1 MiB LDI stream through the ABI | 2.61-2.69 ms | 2.58-2.62 ms (no change) |

After a compressed block, the reset still clears what the trees wrote: there is nothing to gain there, and nothing is
lost. The rest of the call is the contexts' lock and look-up, which every LDI call pays. 0 B, 0 allocations before
and after.

Checked by `sys_ldi_tests`:
- every stream (64 of them, windows 2^15-2^21) is decoded, then decoded again after `Init` resets the decoder;
- new: one decoder through a 2^21 stream, a 2^15 stream, the 2^21 stream again and a plain `Reset`, each giving the
  original bytes.

`sys_adapter_tests` decodes the same chunk again after `LDIResetDecompression`.
