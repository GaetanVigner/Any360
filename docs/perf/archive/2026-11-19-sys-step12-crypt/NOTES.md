# Step 12, seventh batch: 11 cryptography and key calls

The calls Xenia implements and the reference runtime stubs (docs/SYS_STUBS.md, "crypt"): `XeCryptSha512Init`,
`XeCryptSha512Update`, `XeCryptSha512Final`, `XeCryptBnQw_Copy`, `XeCryptBnQwNeModInv`, `XeCryptBnQwNeModMul`,
`XeCryptBnQwBeSigFormat`, `XeCryptBnDwLePkcs1Format`, `XeKeysGetConsoleType`, `XeKeysGetConsoleID`,
`XeKeysGetKeyProperties`. Measured on 2026-11-19 (AMD Ryzen 7 7800X3D) with `sys_abi_bench 9` (the original through
the PowerPC ABI) and `sys_step12_bench 9` (against the host), three times each: `sys_abi_bench_run1-3.txt`,
`sys_step12_bench_run1-3.txt`. The first measure, before the optimizations below, is in `before/`; the variant labs
(sources and outputs) in `lab/`.

Reused, not recoded: the SHA-1 and its paths (`XeCryptBnQwBeSigFormat`'s hash), the SHA path switch (`ShaPath`:
`kPortable` also forces SHA-512's portable code), the number layout of `XeCryptBnQwNeRsaPubCrypt`.
Host libc has no SHA-512, Montgomery product or RC4; CNG has SHA-512 (one call only: the guest's streamed state cannot
be handed to it), so it is the host column.

Against the original (the reference's stub, 65-70 ns a call, doing none of the work; Any360 ns over three runs):

| call | original ns | Any360 ns | speed | B, allocs a call |
|---|---|---|---|---|
| XeKeysGetKeyProperties | 67-69 | 2.0-2.5 | 27-34x | 0, 0 |
| XeKeysGetConsoleType | 67-69 | 2.6-2.7 | 25-26x | 0, 0 |
| XeKeysGetConsoleID | 68-69 | 2.7-2.9 | 23-25x | 0, 0 |
| XeCryptBnQwNeModInv | 65-69 | 3.1 | 21-22x | 0, 0 |
| XeCryptBnQw_Copy (16 words) | 67-70 | 3.8-4.1 | 16-18x | 0, 0 |
| XeCryptSha512Init | 66-70 | 4.7-5.0 | 13-15x | 0, 0 |
| XeCryptBnDwLePkcs1Format (256 B) | 67-70 | 8.3-8.9 | 7.6-8.5x | 0, 0 |
| XeCryptSha512Final | 67-70 | 152-153 | 0.4-0.5x | 0, 0 |
| XeCryptSha512Update (256 B) | 66-69 | 276-280 | 0.2x | 0, 0 |
| XeCryptBnQwNeModMul (1024 bits) | 67-70 | 391-394 | 0.2x | 0, 0 |
| XeCryptBnQwBeSigFormat | 67-70 | 871-883 | 0.1x | 0, 0 |

The four rows under 1x do the work the stub skips (it returns at once, leaving the title's buffers as they were): a
SHA-512 compression is 80 rounds of 64-bit arithmetic, a 1024-bit Montgomery product 512 64-bit products, the
signature block a SHA-1 and an RC4 over 491 bytes. Their cost is the work's own; it was made as fast as measured below.

Against the host (`sys_step12_bench`, ns a call, the fastest of 9 runs, three runs):

| work | bytes | Any360 ns | host ns | speed |
|---|---|---|---|---|
| SHA-512 Init + Update + Final against CNG's BCryptHash | 0 | 155-157 | 255-264 | 1.65-1.68x |
| | 64 | 172-182 | 264-267 | 1.47-1.54x |
| | 256 | 427-430 | 583-588 | 1.35-1.37x |
| | 4096 | 4362-4381 | 5328-5350 | 1.22x |
| | 65536 | 67462-67513 | 81348-81477 | 1.20-1.21x |
| XeCryptBnQw_Copy against memmove | 128 | 3.0-3.2 | 2.3 | 0.72-0.77x |
| | 512 | 4.0-4.8 | 3.5-4.2 | 0.88-0.91x |

- **SHA-512**: the first version (portable C++, `before/`) was 0.75-0.90x CNG from 256 bytes. Measured variants
  (`lab/sha512_variants.*`, 64 KiB): Maj's operand reused from round to round 1.05x, the rounds on BMI2's `rorx` no
  more, the schedule in SSE registers two words at a time 1.3x, both together (the schedule kept in eight registers,
  no array) **1.6x**; AVX-512's `vprorq` nothing more, SSSE3 without BMI2 1.15x. Kept: BMI2 + SSSE3 (no VEX, so no
  operating system check), chosen at start from CPUID (Haswell and later, Zen 2 handhelds and later), the portable
  code (with the Maj reuse) elsewhere; both checked by the tests.
- **XeCryptBnQwNeModMul**: each step waits on its carry and on the word the previous pass wrote, about 7 cycles a step;
  128-bit sums or `mulx` / `adc` intrinsics changed nothing (`lab/modmul_variants.*`), unrolling the inner loop by
  four lets the processor overlap more of the next pass: 1024 bits 387-400 to 382-383 ns, 2048 bits 1456-1473 to
  1366-1367 ns (1.07x), 4096 bits 5681-5884 to 5176-5264 ns (1.10x). No host library offers this product (CNG's RSA takes whole keys).
- **XeCryptBnQwBeSigFormat**: RC4's table in 32-bit words and the key index without a division: 920-932 to 866-883 ns
  (RC4's swaps are a serial chain: 491 of them).
- **XeCryptBnQw_Copy** is `memmove` behind a call into the library (the host row is `memmove` inlined into the timing
  loop): the 0.7 ns between them is that call, which a title pays anyway through the ABI (16-18x the stub).
- **Host memory**: 0 B and 0 allocations a call for every row (`XeCryptBnQwNeModMul` keeps numbers up to 4096 bits
  on the stack; larger ones take one host buffer a call).

The answers (Xenia's xboxkrnl_crypt.cc, with ExCrypt's formats):

- SHA-512's state as the kernel's (`XECRYPT_SHA512_STATE`, 0xC8 bytes): a 32-bit byte count, the eight words big-endian,
  the 128-byte buffer; Final leaves the count, the digest in the words and the last padded block in the buffer, and
  writes the bit count as the low 32 bits of `count << 3`, as the reference does.
- `XeCryptBnQwNeModInv` is the 2-adic inverse (four Newton steps, the same arithmetic as Xenia's for every word);
  `XeCryptBnQwNeModMul` keeps the kernel's two sums and its final correction (an output may not be fully reduced when
  the inputs were not), so every output word equals Xenia's.
- `XeCryptBnQwBeSigFormat`: 0 padding, 1, the salt, SHA-1(8 zero bytes, hash, salt), 0xBC; RC4 under that SHA-1 over the
  bytes before it, the top bit cleared, the 32 words stored least significant first. `XeCryptBnDwLePkcs1Format`: the
  PKCS #1 v1.5 block for a SHA-1 hash, little-endian, DigestInfo formats 0 (standard), 1 (no NULL), 2 (none); sizes
  outside 39-512 all 0xFF.
- The console: retail (2), Xenia's console id (5 bytes and "245149100000", unterminated), the key vault's 57 entry
  sizes and its signature (0x44, 256 bytes), 0 for anything else.

Checked by `sys_object_tests` (`CryptStep12SeventhTests`): FIPS 180-4's SHA-512 vectors (one block, empty, two padding
blocks) on the portable and the BMI2 code, lengths 0-400 streamed in three random parts against the one-call digest
and against a byte-by-byte stream (count, words, buffered bytes), Final's state bytes; ModInv against Xenia's code for
10,000 words and as an inverse; ModMul against Xenia's code for 1-70 words (past the stack's 64) with the output over
each input, and the arithmetic for one word (out * 2^64 = a * b mod m); the signature block undone with its own RC4 key;
every byte of the PKCS #1 blocks; the console id against Xenia's bit-field structure; the key sizes; 0 allocations.
`sys_step12_bench` checks every SHA-512 digest against CNG's.
