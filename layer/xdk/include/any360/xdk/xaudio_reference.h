// The XAudio functions of xaudio.h as their PowerPC code does them, instruction by instruction, one element at a
// time: the models the unit tests compare the replacements with. Written for clarity, not speed. x86-64 with clang
// or GCC (the fused multiply-add is the host instruction the titles' recompiled vmaddfp uses).
#pragma once

#include <cstdint>

#include "xaudio.h"

namespace any360::xdk::reference {

#ifdef ANY360_XDK_XAUDIO_X64
namespace detail_xaudio {

using Vector = uint32_t[4];

inline void Load(const Memory& memory, uint32_t address, Vector& v) {  // lvx128
  const uint8_t* p = memory.Host(address & ~0xFu);
  for (int i = 0; i < 4; ++i) v[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
}
inline void Store(const Memory& memory, uint32_t address, const Vector& v) {  // stvx
  uint8_t* p = memory.Host(address & ~0xFu);
  for (int i = 0; i < 4; ++i) {
    for (int b = 0; b < 4; ++b) p[4 * i + b] = uint8_t(v[i] >> (8 * (3 - b)));
  }
}
inline void Copy(Vector& d, const Vector& s) {  // vor d,s,s
  for (int i = 0; i < 4; ++i) d[i] = s[i];
}
__attribute__((target("avx,fma"))) inline uint32_t AddElement(uint32_t a, uint32_t b) {
  float x = std::bit_cast<float>(a);
  const float y = std::bit_cast<float>(b);
  __asm__("vaddss %2, %1, %0" : "=x"(x) : "x"(x), "x"(y));
  return std::bit_cast<uint32_t>(x);
}
__attribute__((target("avx,fma"))) inline uint32_t MaddElement(uint32_t a, uint32_t b, uint32_t c) {
  float x = std::bit_cast<float>(a);
  const float y = std::bit_cast<float>(b), z = std::bit_cast<float>(c);
  __asm__("vfmadd213ss %2, %1, %0" : "+x"(x) : "x"(y), "x"(z));
  return std::bit_cast<uint32_t>(x);
}
inline void Vaddfp(Vector& d, const Vector& a, const Vector& b) {
  Vector t;
  for (int i = 0; i < 4; ++i) t[i] = AddElement(a[i], b[i]);
  Copy(d, t);
}
inline void Vmaddfp(Vector& d, const Vector& a, const Vector& c, const Vector& b) {  // vmaddfp d,a,c,b: a * c + b
  Vector t;
  for (int i = 0; i < 4; ++i) t[i] = MaddElement(a[i], c[i], b[i]);
  Copy(d, t);
}

}  // namespace detail_xaudio

// XAudio's mix with a volume ramp, its 64 instructions in order (the cache touches change nothing but r9-r11).
inline void XAudioMixRamp(const Memory& memory, VmxRegisters& x) {
  using namespace detail_xaudio;
  uint64_t* r = x.gpr;
  auto& v = x.vr;
  const auto u32 = [](uint64_t value) { return uint32_t(value); };
  // std r31,-8(r1)
  for (int i = 0; i < 8; ++i) *memory.Host(u32(r[1]) - 8 + i) = uint8_t(r[31] >> (8 * (7 - i)));
  r[10] = u32(r[7]) >> 5;  // rlwinm r10,r7,27,7,31
  r[11] = 0;
  if (u32(r[10]) != 0) {
    do {
      r[9] = (u32(r[11]) << 7) & 0xFFFFFF80u;  // rlwinm r9,r11,7,0,24; dcbt r9,r3
      r[11] = r[11] + 1;
    } while (u32(r[11]) < u32(r[10]));
  }
  Load(memory, u32(r[6]), v[0]);
  r[11] = u32(r[7]) >> 2;  // rlwinm r11,r7,30,2,31
  Vaddfp(v[11], v[0], v[0]);
  Load(memory, u32(r[5]), v[13]);
  Vaddfp(v[12], v[13], v[0]);
  Vaddfp(v[10], v[11], v[0]);
  Vaddfp(v[0], v[11], v[11]);
  Vaddfp(v[11], v[13], v[11]);
  Vaddfp(v[10], v[13], v[10]);
  if (u32(r[11]) != 0) {
    r[11] = r[11] - 1;
    r[10] = r[3] + 48;
    r[9] = u32(r[11]) >> 2;  // rlwinm r9,r11,30,2,31
    r[11] = r[4] + 32;
    r[5] = r[3] - r[4];
    r[9] = r[9] + 1;
    do {
      r[4] = r[11] + 16;
      Copy(v[1], v[10]);
      r[8] = r[10] - 48;
      Copy(v[31], v[13]);
      r[3] = r[11] - 32;
      Copy(v[30], v[12]);
      r[7] = r[10] - 32;
      Copy(v[29], v[11]);
      r[6] = r[5] + r[11];
      Load(memory, u32(r[10]), v[9]);
      r[31] = r[11] - 16;
      Load(memory, u32(r[4]), v[7]);
      Load(memory, u32(r[8]), v[4]);
      Vmaddfp(v[9], v[7], v[1], v[9]);
      Load(memory, u32(r[3]), v[6]);
      r[9] = r[9] - 1;
      Load(memory, u32(r[11]), v[8]);
      Vmaddfp(v[7], v[6], v[31], v[4]);
      Load(memory, u32(r[7]), v[3]);
      r[11] = r[11] + 64;
      Load(memory, u32(r[6]), v[2]);
      Vaddfp(v[13], v[13], v[0]);
      Load(memory, u32(r[31]), v[5]);
      Vmaddfp(v[8], v[8], v[29], v[2]);
      Vmaddfp(v[6], v[5], v[30], v[3]);
      Vaddfp(v[12], v[12], v[0]);
      Vaddfp(v[11], v[11], v[0]);
      Vaddfp(v[10], v[10], v[0]);
      Store(memory, u32(r[10]), v[9]);
      r[10] = r[10] + 64;
      Store(memory, u32(r[8]), v[7]);
      Store(memory, u32(r[6]), v[8]);
      Store(memory, u32(r[7]), v[6]);
    } while (u32(r[9]) != 0);
  }
  // ld r31,-8(r1)
  uint64_t saved = 0;
  for (int i = 0; i < 8; ++i) saved = saved << 8 | *memory.Host(u32(r[1]) - 8 + i);
  r[31] = saved;
}
#endif

// XAudio's fill (TDU's sub_826B9E38 and its four siblings), instruction by instruction: `rounds` rounds of eight
// stores of the float at +4, from +8; then 0 at +0.
inline void XAudioFill(const Memory& memory, uint32_t r3, uint32_t rounds) {
  uint32_t r11 = r3 + 12;
  for (uint32_t r10 = rounds; r10 != 0; --r10) {
    for (int32_t k = -4; k <= 24; k += 4) {
      const uint8_t* from = memory.Host(r3 + 4);  // lfs f0,4(r3): read again for each store
      uint8_t* to = memory.Host(r11 + uint32_t(k));
      for (int b = 0; b < 4; ++b) to[b] = from[b];  // stfs f0: the single's bits
    }
    r11 += 32;
  }
  for (int b = 0; b < 4; ++b) *memory.Host(r3 + b) = 0;  // li r11,0; stw r11,0(r3)
}

}  // namespace any360::xdk::reference
