# FFmpeg (vendored subset)

The part of FFmpeg that the ReXGlue SDK builds for XMA decoding (libavutil and libavcodec: FFmpeg 4.4 as forked by
Xenia, with its `xmaframes` decoder), so that Any360 builds it itself for its XMA decoder (`any360_sys_xma`).

Why: the SDK's prebuilt libraries are compiled without FFmpeg's x86 assembly (`HAVE_X86ASM 0`), so the decoder's
transforms run their C versions. Built here with NASM, the XMA path's modules use FFmpeg's own SSE / AVX code: 20%
less time a frame, the same output bit for bit on a capture of 110108 frames (`docs/perf/archive/2026-11-20-xma-ffmpeg-asm`).

## Source

Copied unchanged from the ReXGlue SDK's `thirdparty/FFmpeg` and `thirdparty/ffmpeg-overlay` (the SDK 0.8.0 source tree
in The Simpsons Game port; the SDK 0.10.0 libraries Crackdown links define the same symbols, and decode the capture to
the same hash). https://github.com/xenia-project/FFmpeg is the fork's origin, https://ffmpeg.org FFmpeg's.

Files:
- The 170 C files the SDK compiles for x86-64 (`CMakeLists.txt` here lists them, as the SDK's own CMake does) and the
  headers they include (the list of a full build's dependencies).
- FFmpeg's x86 assembly for the XMA path, with its macro headers: `libavutil/x86/cpuid.asm`,
  `libavutil/x86/float_dsp.asm`, `libavcodec/x86/fft.asm`, `libavutil/x86/x86inc.asm`, `libavutil/x86/x86util.asm`.
- `config_windows_x86_64.h`, `config_linux_x86_64.h`: the SDK's committed configure outputs.
- `overlay/`: the SDK's generated codec, parser and bitstream filter lists.
- `compat/atomics/gcc/stdatomic.h` (Linux builds).

Not copied: anything the SDK does not build, its ARM64 code (an ARM64 tree keeps the SDK's libraries).

## What Any360 adds

Only `CMakeLists.txt`.
- It builds the SDK's file set as `any360_avutil` and `any360_avcodec`.
- With NASM (`ANY360_NASM`, found on the PATH or given), it also builds five modules against a copy of the config
  with x86 assembly on: CPU detection, float DSP, their x86 init, FFT / MDCT and its x86 init. It assembles their
  three `.asm` files, with a `config.asm` generated from the config, `HAVE_CPUNOP` off (NASM 2.16 lacks
  `CPU amdnop`).
- Without NASM it is the SDK's C build.
- `ANY360_FFMPEG_VENDORED=OFF` (layer/sys) goes back to the SDK's prebuilt libraries.

## License

FFmpeg is licensed under the GNU Lesser General Public License version 2.1 or later (`COPYING.LGPLv2.1`, copied
unchanged); the files carry their own notices. Any360 links it statically, as the ReXGlue SDK does.
