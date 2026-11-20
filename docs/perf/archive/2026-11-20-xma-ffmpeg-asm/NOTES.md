# 2026-11-20: Any360 builds the SDK's FFmpeg itself, with FFmpeg's x86 assembly on the XMA path

Follows 2026-11-19-xma-output-conversion. That step's profile of the frame decoder put about 31% in FFmpeg's
transforms and windows, all in their C versions:
- The ReXGlue SDK builds FFmpeg (4.4, Xenia's fork with `xmaframes`) with `HAVE_X86ASM 0` and none of its x86
  assembly, so `av_get_cpu_flags()` is 0 on Windows and no SSE / AVX code is ever chosen.
- On Linux the flags are detected (inline assembly), but there is no assembly to choose either.

The owner chose to build FFmpeg's own x86 assembly (NASM), with the sources copied into Any360.

## What was tried first (scratch builds, `bench.txt`)

- The SDK's sources rebuilt with clang: 3% faster than the SDK's libraries, the same output.
- The same with `-march=x86-64-v3` (auto-vectorized, `-ffp-contract=off` to keep the bits): no gain. The transforms'
  C code does not vectorize.
- FFmpeg's x86 assembly for CPU detection (`cpuid.asm`), FFT / MDCT (`fft.asm`) and float DSP (`float_dsp.asm`):
  -20%.
  - The output is the same bit for bit: the capture's hash is unchanged.
  - With the CPU flags forced to SSE only (no AVX), still -20%.

## The change

`layer/sys/third_party/ffmpeg` (NOTICE.md): the files the SDK compiles (170 C files, the headers they include, its
configs and generated lists, LGPL 2.1), copied unchanged from the SDK's tree, and a CMakeLists.txt.
- It builds `any360_avutil` / `any360_avcodec` from the SDK's own source lists.
- With NASM (`ANY360_NASM`), it builds the XMA path's five modules against a config with x86 assembly on, and
  assembles their three `.asm` files (`config.asm` generated, `HAVE_CPUNOP` off: NASM 2.16 lacks `CPU amdnop`).
- Without NASM it is the SDK's C build.

Elsewhere:
- `any360_sys_xma` links it on x86-64 (`ANY360_FFMPEG_VENDORED`, on). An SDK built in a title's tree keeps its own
  FFmpeg targets, and ARM64 keeps the SDK's libraries.
- The game's executable got FFmpeg only through `any360_sys_xma` (the runtime's copy lives in its DLL), so nothing
  is linked twice.
- `XmaFrameDecoder::SimdFlags()` reports what FFmpeg detects. `ANY360_XMA_NO_SIMD=1` forces FFmpeg's C versions in
  the same build.
- The suite runs `xma_tests` now that the layer's own tree builds FFmpeg (`test.ps1 -Nasm`). The binding's SDL audio
  output is only built inside a title (`ANY360_HOST_AUDIO`), since the layer's own tree has no SDL.

## Tests

- `xma_tests`: 0 failing. It reports the flags and checks that they are non-zero when the assembly is built.
- Windows: the capture's hash is unchanged (47221808C06B2A03) with the SDK's libraries, with the vendored C build,
  and with the vendored assembly.
- Linux (WSL Ubuntu 20.04, clang-20, NASM 2.14 `-f elf64`): built and linked, `ff_imdct_half_avx` and the others in
  the binary, xma_tests 0 failing. Its hash (292DDB00C3799985) is the SDK's own Linux libraries' too. Windows and
  Linux already differed before this change: the C libraries' `cos()` that fills FFmpeg's tables.

## Bench (`bench.txt`; xma_bench on the capture, ns a frame)

| | Windows | Linux (WSL) |
|---|---:|---:|
| the SDK's prebuilt libraries | 6881-6977 | 7861-7930 |
| vendored, C (`ANY360_XMA_NO_SIMD=1`) | 6631-6829 | 7068-8289 |
| **vendored, x86 assembly** | **5533-5620** | **5860-6237** |

- That is -20% on Windows and -25% on Linux against the SDK.
- 14.06 C runtime allocations a frame, FFmpeg's own, as before.

## In game (Radeon, cores 8-15, the city route; the same build, `ANY360_XMA_NO_SIMD` alternating, 5 v 5)

`ANY360_XMA_STATS` at 20000 kicks, nanoseconds a frame in the frame decoder:

| | launches | median |
|---|---|---:|
| x86 assembly (flags 0x1FD3DB) | 8017, 8115, 7412, 7441, 7358 | 7441 |
| C (flags 0) | 8982, 9186, 9549, 9263, 9630 | 9263 |

That is -19.7%; every assembly launch is below every C launch.

Frame rate (`ab_lows.txt`, steady periods):

| | assembly | C |
|---|---:|---:|
| FPS (mean / median) | 62.6 / 62.9 | 63.0 / 62.7 |
| 10% low | 62.0 | 62.3 |
| 1% low (mean / median) | 61.0 / 61.6 | 60.9 / 61.3 |
| private memory | 2567 MB | 2563 MB |

No difference (rank z -1.15 to +0.08): the gain is processor time on the title's audio thread.

## Builds

- A title gets the assembly when NASM is on its PATH or it passes `-DANY360_NASM=<nasm>`. Here that is
  `F:\Crackdown\tools\nasm\nasm-2.16.03\nasm.exe` (Crackdown's build-ab was reconfigured with it).
- In WSL the Ubuntu package (`apt-get install nasm`).
- Without NASM a title builds as before, in C.
