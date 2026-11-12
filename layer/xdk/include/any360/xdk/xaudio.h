// any360::xdk: native replacements of functions of the XDK's audio library (XAudio). The rule they follow:
// docs/NATIVE_FUNCTIONS.md.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#include "crt.h"

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__clang__) || defined(__GNUC__))
#include <immintrin.h>
#define ANY360_XDK_XAUDIO_X64 1
#endif

namespace any360::xdk {

// The registers a vector kernel reads and leaves: the general ones as the title's registers hold them (64 bits),
// the vector ones in the guest's element order (element 0 first), each element a float's bits.
struct VmxRegisters {
  uint64_t gpr[32] = {};
  uint32_t vr[32][4] = {};
};

// XAudio's mix with a volume ramp (named after what it does; its name is not in the titles): r3 the destination,
// r4 the source, r5 and r6 the addresses of the starting gains and of their step per vector of four samples, r7
// the count of samples. For each vector k of four samples, destination[k] = source[k] * gain[k] + destination[k],
// fused (vmaddfp, one rounding, as the console does), gain[k] = gain + k * step kept in four running sums of
// four steps each. Rounded up to sixteen samples: it goes by four vectors at a time, loading all eight before it
// stores the four sums, and the vectors' addresses are rounded down to 16 bytes as lvx128 does. It saves r31 at
// r1 - 8 (std r31,-8(r1)) and leaves r3-r11 and v0-v13, v29-v31 as its steps do; it runs under the vector unit's
// flush mode (denormal inputs read as zero, denormal results become zero), which the caller sets on the host.
//
// Declined (nothing written): a host without FMA3 (the fused multiply-add the title's recompiled code uses), or
// memory that is not plain.
namespace detail {

constexpr uint32_t kMixRampVector = 16, kMixRampGroup = 64;

// The iterations (groups of four vectors) for a count of samples; 0 below four.
inline uint32_t MixRampGroups(uint32_t samples) {
  const uint32_t vectors = samples >> 2;
  return vectors ? ((vectors - 1) >> 2) + 1 : 0;
}

inline uint32_t FloatBits(float f) { return std::bit_cast<uint32_t>(f); }
inline float BitsFloat(uint32_t b) { return std::bit_cast<float>(b); }

#ifdef ANY360_XDK_XAUDIO_X64
inline bool HostHasFma() {
  static const bool has = [] {
    uint32_t a, b, c, d;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    constexpr uint32_t kFmaOsxsaveAvx = (1u << 12) | (1u << 27) | (1u << 28);
    if ((c & kFmaOsxsaveAvx) != kFmaOsxsaveAvx) return false;
    uint32_t lo, hi;
    __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0u));
    return (lo & 6u) == 6u;
  }();
  return has;
}

// Guest vectors (16 big-endian bytes) to and from host vectors in the guest's element order.
__attribute__((target("avx,fma"))) inline __m128 LoadGuestVector(const uint8_t* p, __m128i swap) {
  return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), swap));
}
__attribute__((target("avx,fma"))) inline void StoreGuestVector(uint8_t* p, __m128 v, __m128i swap) {
  _mm_storeu_si128(reinterpret_cast<__m128i*>(p), _mm_shuffle_epi8(_mm_castps_si128(v), swap));
}
__attribute__((target("avx,fma"))) inline void ToRegister(uint32_t (&r)[4], __m128 v) {
  _mm_storeu_si128(reinterpret_cast<__m128i*>(r), _mm_castps_si128(v));
}
__attribute__((target("avx,fma"))) inline __m128 FromRegister(const uint32_t (&r)[4]) {
  return _mm_castsi128_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(r)));
}
// a * b + c rounded once, the instruction (and so the NaN that wins) of the title's recompiled vmaddfp.
__attribute__((target("avx,fma"))) inline __m128 FusedMultiplyAdd(__m128 a, __m128 b, __m128 c) {
  __asm__("vfmadd213ps %2, %1, %0" : "+x"(a) : "x"(b), "x"(c));
  return a;
}
__attribute__((target("avx,fma"))) inline __m128 Add(__m128 a, __m128 b) {
  __asm__("vaddps %2, %1, %0" : "=x"(a) : "x"(a), "x"(b));
  return a;
}

__attribute__((target("avx,fma"))) inline void MixRampKernel(const Memory& memory, VmxRegisters& x) {
  uint64_t* r = x.gpr;
  const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  const uint32_t samples = uint32_t(r[7]);
  const uint64_t dest = r[3], source = r[4];
  // The cache-touch loop leaves r9, r10, r11.
  const uint32_t lines = samples >> 5;
  if (lines) r[9] = uint64_t(((lines - 1) << 7) & 0xFFFFFF80u);
  r[10] = lines;
  r[11] = lines;
  const __m128 step = LoadGuestVector(memory.Host(uint32_t(r[6]) & ~0xFu), swap);
  const __m128 g0_start = LoadGuestVector(memory.Host(uint32_t(r[5]) & ~0xFu), swap);
  const __m128 two = Add(step, step);
  __m128 g1 = Add(g0_start, step);
  const __m128 three = Add(two, step);
  const __m128 step4 = Add(two, two);
  __m128 g2 = Add(g0_start, two);
  __m128 g3 = Add(g0_start, three);
  __m128 g0 = g0_start;
  r[11] = samples >> 2;
  const uint32_t groups = MixRampGroups(samples);
  if (groups) {
    uint8_t* d = memory.Host(uint32_t(dest) & ~0xFu);
    const uint8_t* s = memory.Host(uint32_t(source) & ~0xFu);
    __m128 last_g0 = g0, last_g1 = g1, last_g2 = g2, last_g3 = g3;
    __m128 d0 = g0, d1 = g0, d2 = g0, s1 = g0, m0 = g0, m1 = g0, m2 = g0, m3 = g0;
    for (uint32_t i = 0; i < groups; ++i, d += kMixRampGroup, s += kMixRampGroup) {
      last_g0 = g0;
      last_g1 = g1;
      last_g2 = g2;
      last_g3 = g3;
      d0 = LoadGuestVector(d, swap);
      d1 = LoadGuestVector(d + 16, swap);
      d2 = LoadGuestVector(d + 32, swap);
      const __m128 d3 = LoadGuestVector(d + 48, swap);
      const __m128 s0 = LoadGuestVector(s, swap);
      s1 = LoadGuestVector(s + 16, swap);
      const __m128 s2 = LoadGuestVector(s + 32, swap), s3 = LoadGuestVector(s + 48, swap);
      m3 = FusedMultiplyAdd(s3, g3, d3);
      m0 = FusedMultiplyAdd(s0, g0, d0);
      m2 = FusedMultiplyAdd(s2, g2, d2);
      m1 = FusedMultiplyAdd(s1, g1, d1);
      g0 = Add(g0, step4);
      g1 = Add(g1, step4);
      g2 = Add(g2, step4);
      g3 = Add(g3, step4);
      StoreGuestVector(d + 48, m3, swap);
      StoreGuestVector(d, m0, swap);
      StoreGuestVector(d + 32, m2, swap);
      StoreGuestVector(d + 16, m1, swap);
    }
    const uint64_t last = 64ull * (groups - 1);
    r[3] = source + last;
    r[4] = source + last + 48;
    r[5] = dest - source;
    r[6] = (dest - source) + (source + 32 + last);
    r[7] = dest + 48 + last - 32;
    r[8] = dest + 48 + last - 48;
    r[9] = 0;
    r[10] = dest + 48 + 64ull * groups;
    r[11] = source + 32 + 64ull * groups;
    ToRegister(x.vr[1], last_g3);
    ToRegister(x.vr[2], d2);
    ToRegister(x.vr[3], d1);
    ToRegister(x.vr[4], d0);
    ToRegister(x.vr[5], s1);
    ToRegister(x.vr[6], m1);
    ToRegister(x.vr[7], m0);
    ToRegister(x.vr[8], m2);
    ToRegister(x.vr[9], m3);
    ToRegister(x.vr[29], last_g2);
    ToRegister(x.vr[30], last_g1);
    ToRegister(x.vr[31], last_g0);
  }
  ToRegister(x.vr[0], step4);
  ToRegister(x.vr[10], g3);
  ToRegister(x.vr[11], g2);
  ToRegister(x.vr[12], g1);
  ToRegister(x.vr[13], g0);
}
#endif

}  // namespace detail

// What the mix reads and writes: its destination's groups, its source's, the two gain vectors, the stack slot.
inline bool XAudioMixRampAccepts(const VmxRegisters& x) {
#ifdef ANY360_XDK_XAUDIO_X64
  if (!detail::HostHasFma()) return false;
  const uint32_t bytes = detail::MixRampGroups(uint32_t(x.gpr[7])) * detail::kMixRampGroup;
  const uint32_t dest = uint32_t(x.gpr[3]) & ~0xFu, slot = uint32_t(x.gpr[1]) - 8;
  // The slot r31 is saved to and read back from, apart from what the mix writes.
  return IsPlainMemory(dest, bytes) && IsPlainMemory(uint32_t(x.gpr[4]) & ~0xFu, bytes) &&
         IsPlainMemory(uint32_t(x.gpr[5]) & ~0xFu, 16) && IsPlainMemory(uint32_t(x.gpr[6]) & ~0xFu, 16) &&
         IsPlainMemory(slot, 8) && (slot >= dest + bytes || slot + 8 <= dest);
#else
  (void)x;
  return false;
#endif
}
// The bytes it writes in its destination: [at, at + size).
inline Range XAudioMixRampWrite(const VmxRegisters& x) {
  return {uint32_t(x.gpr[3]) & ~0xFu, detail::MixRampGroups(uint32_t(x.gpr[7])) * detail::kMixRampGroup};
}

inline bool XAudioMixRamp(const Memory& memory, VmxRegisters& x) {
#ifdef ANY360_XDK_XAUDIO_X64
  if (!XAudioMixRampAccepts(x)) return false;
  detail::Store64(memory.Host(uint32_t(x.gpr[1]) - 8), detail::ByteSwap64(x.gpr[31]));
  detail::MixRampKernel(memory, x);
  return true;
#else
  (void)memory, (void)x;
  return false;
#endif
}

// XAudio's fill of a block's buffer with its value (named after what it does; five builds, one a size: 256, 512,
// 1024, 2048 and 4096 floats, the same steps in the three titles at hand): r3 the block, a word, a float at +4, the
// buffer from +8. Eight floats a round, each the block's float read again (lfs) and stored (stfs), then the block's
// first word set to 0. lfs and stfs move a single's bits unchanged on the console (a load converts nothing that
// matters for a single stored back): the replacement copies the bits. Leaves f1-f31 alone and f0 the value as a
// double. Declined: memory that is not plain.
inline bool XAudioFillAccepts(uint32_t block, uint32_t floats) {
  return floats <= (1u << 20) && IsPlainMemory(block, 8 + 4 * floats);
}
inline bool XAudioFill(const Memory& memory, uint32_t block, uint32_t floats, uint32_t& value_bits) {
  if (!XAudioFillAccepts(block, floats)) return false;
  uint32_t raw;  // the value as guest memory holds it (big-endian): stored as it is
  std::memcpy(&raw, memory.Host(block + 4), 4);
  uint8_t* to = memory.Host(block + 8);
  for (uint32_t i = 0; i < floats; ++i) std::memcpy(to + 4 * i, &raw, 4);
  detail::Store32(memory.Host(block), 0);
  value_bits = std::byteswap(raw);
  return true;
}

}  // namespace any360::xdk
