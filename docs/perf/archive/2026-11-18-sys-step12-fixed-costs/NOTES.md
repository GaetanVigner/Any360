# Step 12: the hashes' and ciphers' fixed costs

[2026-11-18-sys-step12-abi](../2026-11-18-sys-step12-abi/) showed the hashes and ciphers slower than the original:
the original is the reference's stub, which returns at once without computing anything (70 ns), so no real hash can
match it; the work itself was already at or above the host's own code (CNG) in
[2026-11-18-sys-step12-opt](../2026-11-18-sys-step12-opt/): MD5 750 MB/s like CNG, SHA-1 and SHA-256 on the
processor's SHA instructions. What could still go is the fixed cost of each call. Measured on 2026-11-18 with
`sys_step12_bench 9` and `sys_abi_bench 9`, three times each, on cores 0-7. Raw output: `sys_step12_bench_run1-3.txt`,
`sys_abi_bench_run1-3.txt`.

What changed:

- **DES key schedule on byte tables**: PC1 and PC2 are bit permutations, so each is the OR of one table entry per
  input byte (tables built at compile time from the standard's); the rotations of C and D in one step.
- **HMAC pad states kept for the last key**: an HMAC compresses the key XOR 0x36 and XOR 0x5C before any data. Each
  thread keeps the two states of the last key it used (64 bytes or fewer; compared byte for byte), so the same key
  again skips both compressions (`XeCryptHmacMd5`, `XeCryptHmacSha`, `XeCryptHmacShaVerify`, `XeCryptHmacShaInit`,
  `XeKeysHmacSha*`). A title signs or checks many messages under one key; a new key costs what it did.
- **Single DES CBC decryption two blocks at a time** (each block depends only on the ciphertext). The same for triple
  DES was slower (23.0 to 27.4 µs at 1 KB) and is not kept.

Before → after (ns a call; median of the three runs):

| call | before | after | gain |
|---|---|---|---|
| XeCryptDesKey | 560 | 43 | 13x |
| XeCryptDes3Key | 1,580 | 132 | 12x |
| XeCryptHmacShaInit | 48 | 12 | 4.0x |
| HMAC-SHA-1 Init+Update+Final, 0 B / 256 B | 146 / 246 | 98 / 202 | 1.5x / 1.2x (4.1-4.4x / 3.4x CNG) |
| XeCryptHmacShaVerify, 0 B / 256 B | 127 / 234 | 88 / 181 | 1.4x / 1.3x |
| XeCryptHmacMd5, 0 B / 256 B | 353 / 700 | 196 / 542 | 1.8x / 1.3x (2.3x / 1.5x CNG) |
| XeCryptHmacMd5 with a new key each call, 0 B / 256 B | 353 / 700 | 371 / 717 | none (as before) |
| XeCryptDesCbc decrypt, 1 KB | 8,327 | 5,682 | 1.5x |
| XeCryptDes3Cbc decrypt, 1 KB | 22,986 | 23,041 | unchanged |

Through the ABI against the original (the stub, about 69 ns): `XeCryptDesKey` now 1.4x faster than the stub,
`XeCryptHmacShaInit` 5.6x, `XeCryptMd5Init` 23x; the hashes and ciphers over 256 bytes cost what the work costs
(`XeCryptMd5` 430 ns, `XeCryptHmacMd5` 530 ns, `XeCryptDesCbc` 2.2 µs, `XeCryptSha256` 150 ns), the stub doing none.

- **Where the remaining time is the work's own bound**: MD5 is one serial chain of 64 steps a block (Any360 and CNG
  both about 750 MB/s); DES CBC encryption is one serial chain of 16 rounds a block (CNG the same); SHA-1 and SHA-256
  run on the processor's SHA instructions (2.4-2.6 GB/s).
- **Host memory**: 0 B and 0 allocations a call everywhere (the kept pad states are 2 x 20 or 2 x 16 bytes of
  thread storage, plus the 64-byte key).

Checked by `sys_object_tests`: the standard vectors on every path, DES and triple DES CBC both ways against the
bitwise model over 1 to 9 blocks (the odd block after the pairs), HMACs under alternating keys (A, B, A, a longer B,
A) each giving its own digest; `sys_io_tests` compares triple DES and the HMACs with the reference runtime.
