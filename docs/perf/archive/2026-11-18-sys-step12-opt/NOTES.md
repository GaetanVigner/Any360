# Step 12, optimization pass: the hashes, DES and lstrlenW against what the host offers

The 27 calls of [2026-11-18-sys-step12](../2026-11-18-sys-step12/) (the "before": same machine, same benchmark) where
Any360 was slower than the host's own code for the same work, made faster; where it could not be made as fast, the
host's code runs behind one switch (`crypt.h`: `ShaPath`). Measured on 2026-11-18 with `sys_step12_bench 9` three
times (fastest of 9 runs each, RelWithDebInfo, clang, the owner's desktop: NVIDIA machine, Windows 11, a processor with
SHA-NI). Raw output: [run 1](sys_step12_bench_run1.txt), [run 2](sys_step12_bench_run2.txt),
[run 3](sys_step12_bench_run3.txt). Speeds below are Any360 / host over the three runs (ratio > 1: Any360 faster).
The host's column moves about 10% between runs; a row within that is parity, not a win.

What changed:

- SHA-1 and SHA-256 compress on the processor's SHA instructions (x86-64 SHA-NI; the Steam Deck's Zen 2 has them),
  detected once by CPUID, the portable C++ kept as the fallback. Every hash's Update now compresses whole blocks straight
  from the input, and Final pads in one block buffer instead of 55 one-byte Updates.
- MD5's rounds are fully unrolled (constant rotations) and each step sums what does not wait on the previous step
  first (G as `(b & d) + (c & ~d)`), so the chain between steps is shorter.
- DES folds the S-boxes and P into eight 64-entry tables, reads E's six-bit groups from two rotations of R, permutes
  IP and FP with five swaps each, and triple DES drops the FP / IP pair between its stages (FP then IP is the identity).
  The guest's schedule format (sixteen little-endian 64-bit subkeys) is unchanged.
- `lstrlenW` looks at the first eight units inline, then runs the host's own 16-bit scan (Windows' `wcslen`): a zero
  unit is zero in either byte order, so it reads the guest's big-endian string as it lies. The "before" notes said
  `wcslen` could not; that was wrong.
- The host switch: `ShaPath::kProcessor` (SHA-NI, the default where the processor has it), `kHost` (Windows without
  SHA-NI: CNG for the one-call SHA-1 / SHA-256 / HMAC-SHA-1 forms of 64 bytes or more; the streamed forms cannot, CNG
  does not resume from a guest state), `kPortable`. Tests and benches force each.

Results (default path; before → after; ns a call at 64 KB unless said):

| call | before (x CNG) | after (x CNG) |
|---|---|---|
| XeCryptSha (SHA-1, new row) | - | 2.8-5.0x at every size (25.5 µs at 64 KB) |
| XeCryptSha256 | 0.15-0.54x | 2.3-2.7x below 64 B, 1.0-1.5x from 256 B, parity (0.95-1.01x) at 64 KB: both on SHA-NI |
| HMAC-SHA-1 streamed (0xB0) | 0.50-0.63x | 2.6-4.1x |
| XeCryptHmacShaVerify | 148.8 µs | 25.8 µs (5.8x faster than before) |
| XeCryptMd5 | 0.94-1.36x | 1.00-1.70x (parity at 64 KB) |
| XeCryptHmacMd5 | 0.94-1.04x | 1.00-1.69x (parity at 64 KB) |
| XeCryptDesCbc | 0.24x | 2.3-3.4x |
| XeCryptDes3Cbc (new row) | - | 0.90-1.17x: parity |
| lstrlenW at 256 characters | 112 ns | 9.8 ns |

- **Host memory**: 0 B and 0 allocations a call for every Any360 row, over 66.7 M timed calls. The allocation meter
  counts `operator new` only, so CNG's own heap use (the host columns and the [host] rows) is not measured.
- **Triple DES stays at parity**: CBC encryption is one serial chain of 48 rounds a block, and CNG's is bound the same
  way. There is no host route to offer: the guest state holds only the schedule, not the key CNG would need.
- **Without SHA-NI** ([C++] rows, the portable path): SHA-1 0.59-1.8x, SHA-256 0.09-0.82x, HMAC-SHA-1 0.61-0.97x. Those
  ratios are against CNG *with* SHA-NI on this machine, so they overstate the gap a processor without SHA-NI would see
  (there CNG runs its own fallback). That gap is not measured: no such machine. On such a processor Windows takes the
  one-call forms from 64 bytes to CNG ([host] rows: the same as CNG within noise, plus one create / hash / finish);
  below 64 bytes the portable code's single block costs less than CNG's call (1.1-1.8x). The streamed forms stay on
  the portable code (the "HmacSha Init+Update+Final [host]" rows are that code).
- **DES decrypt** (new rows): the same as encrypt, 66.6 ns a block single, 181.5 ns triple.

Checked by `sys_object_tests`: every standard vector (RFC 1321, FIPS 180-4, RFC 2202, FIPS 81) on each path the
machine has; SHA-1, SHA-256 and HMAC-SHA-1 (keys of 0 to 100 bytes) one call and streamed on every path against the
portable one byte for byte over lengths 0-200; DES and triple DES against a bitwise FIPS 46-3 model on random keys and
blocks, both ways; `lstrlenW` at every alignment, an odd address, lengths to 300 and units with a zero byte. The suite's
`sys_io_tests` compares SHA-1, SHA-256 and triple DES with the reference runtime (64 trials each, equal). Also built
and run on Linux (WSL Ubuntu 20.04, clang-20): tests pass and SHA-NI is detected there; `lstrlenW` there uses the C++
library's `char_traits<char16_t>::length` (a plain loop in libstdc++), not measured against the old loop.
