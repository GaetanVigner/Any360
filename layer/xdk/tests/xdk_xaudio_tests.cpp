// Unit tests of the XAudio replacements (any360/xdk/xaudio.h) against their models (xaudio_reference.h), which do
// the PowerPC steps one instruction and one element at a time. Every accepted call must leave memory and every
// register the function writes exactly as the model does; a declined call must leave everything untouched.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include <immintrin.h>

#include "any360/xdk/xaudio.h"
#include "any360/xdk/xaudio_reference.h"
#include "xdk_test.h"

namespace {

using any360::xdk::VmxRegisters;
using xdk_test::kStack;
using xdk_test::Pair;
namespace xdk = any360::xdk;

// The vector unit's flush mode, as the title's code sets it before the kernel (FTZ and DAZ).
struct FlushMode {
  unsigned saved = _mm_getcsr();
  FlushMode() { _mm_setcsr(saved | 0x8040); }
  ~FlushMode() { _mm_setcsr(saved); }
};

std::mt19937_64 g_rng(11);

// Floats as a mix's buffers hold them, and now and then the values that round or flush differently.
uint32_t SampleBits(bool special) {
  if (special) {
    switch (g_rng() % 10) {
      case 0: return 0x00000001u;                         // the smallest denormal
      case 1: return 0x807FFFFFu;                         // the largest negative denormal
      case 2: return 0x80000000u;                         // -0
      case 3: return 0x7F800000u;                         // +inf
      case 4: return 0x7FC00001u;                         // a quiet NaN with a payload
      case 5: return 0x7F7FFFFFu;                         // the largest float
      case 6: return 0x00800000u;                         // the smallest normal
      case 7: return uint32_t(g_rng()) & 0x807FFFFFu;     // any denormal
      default: return uint32_t(g_rng());                  // any bits
    }
  }
  const float f = float(int32_t(uint32_t(g_rng()))) / 2147483648.0f;
  return std::bit_cast<uint32_t>(f);
}

void FillVector(Pair& p, uint32_t address, bool special) {
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t bits = SampleBits(special && g_rng() % 4 == 0);
    for (uint32_t b = 0; b < 4; ++b) p.native[address + 4 * i + b] = p.model[address + 4 * i + b] = uint8_t(bits >> (8 * (3 - b)));
  }
}

VmxRegisters RandomRegisters() {
  VmxRegisters x;
  for (uint64_t& r : x.gpr) r = g_rng();
  for (auto& v : x.vr) {
    for (uint32_t& e : v) e = uint32_t(g_rng());
  }
  return x;
}

bool SameRegisters(const VmxRegisters& a, const VmxRegisters& b) {
  return std::memcmp(a.gpr, b.gpr, sizeof a.gpr) == 0 && std::memcmp(a.vr, b.vr, sizeof a.vr) == 0;
}

// One mix on both memories with the same registers. Returns whether the replacement accepted it.
bool Mix(Pair& p, VmxRegisters x) {
  VmxRegisters model = x;
  const bool accepted = xdk::XAudioMixRamp(p.a, x);
  if (!accepted) {
    CHECK(SameRegisters(x, model) && p.SameEverywhere());
    return false;
  }
  xdk::reference::XAudioMixRamp(p.b, model);
  CHECK(SameRegisters(x, model));
  CHECK(p.SameEverywhere());
  return true;
}

// Registers for a call: destination, source, gains and their step, samples; the rest random, upper halves of the
// address registers too (the function's arithmetic on them is 64-bit).
VmxRegisters Call(uint64_t dest, uint64_t source, uint32_t gains, uint32_t step, uint64_t samples) {
  VmxRegisters x = RandomRegisters();
  x.gpr[1] = kStack;
  x.gpr[3] = dest, x.gpr[4] = source, x.gpr[5] = gains, x.gpr[6] = step, x.gpr[7] = samples;
  return x;
}

// Buffers apart: every count of samples up to 200 and some long ones, unaligned addresses, random upper halves.
void Apart() {
  Pair p(3);
  FlushMode flush;
  std::vector<uint32_t> counts;
  for (uint32_t n = 0; n <= 200; ++n) counts.push_back(n);
  for (uint32_t n : {255u, 256u, 257u, 511u, 512u, 1024u, 4096u}) counts.push_back(n);
  for (uint32_t n : counts) {
    for (uint32_t offset = 0; offset < 16; offset += n > 200 ? 5 : 3) {
      const uint32_t dest = 0x10000 + offset, source = 0x40000 + (offset * 7 & 15), gains = 0x80000 + offset, step = 0x80100 + (offset * 3 & 15);
      for (uint32_t i = 0; i < 4096 * 4 + 64; i += 16) FillVector(p, dest - offset + i, false), FillVector(p, (source & ~15u) + i, false);
      FillVector(p, gains & ~15u, false);
      FillVector(p, step & ~15u, false);
      // Steps small enough that the ramp stays a ramp, as XAudio's are.
      for (uint32_t b = 0; b < 16; b += 4) p.native[(step & ~15u) + b] = p.model[(step & ~15u) + b] = 0x38;
      const uint64_t high = (g_rng() % 3 == 0) ? (g_rng() << 32) : 0;
      CHECK(Mix(p, Call(high | dest, high | source, gains, step, n)));
    }
  }
  // The count's upper half is not read (rlwinm takes the low word).
  CHECK(Mix(p, Call(0x10000, 0x40000, 0x80000, 0x80100, 0x100000000ull | 64)));
}

// The values that round or flush differently: denormals in, denormal sums out, infinities, NaNs, overflow.
void SpecialValues() {
  Pair p(5);
  FlushMode flush;
  for (int round = 0; round < 3000; ++round) {
    const uint32_t n = 4 * uint32_t(1 + g_rng() % 40);
    const uint32_t dest = 0x20000, source = 0x30000, gains = 0x80000, step = 0x80100;
    for (uint32_t i = 0; i < 4 * n + 64; i += 16) FillVector(p, dest + i, true), FillVector(p, source + i, true);
    FillVector(p, gains, true);
    FillVector(p, step, round % 2 == 0);
    CHECK(Mix(p, Call(dest, source, gains, step, n)));
  }
}

// The source and the destination overlapping: the function loads a group's eight vectors before it stores four.
void Overlaps() {
  Pair p(7);
  FlushMode flush;
  for (int32_t shift : {0, 16, -16, 32, -32, 48, -48, 64, -64, 80, -80, 8, -8}) {
    for (uint32_t n : {4u, 16u, 20u, 64u, 100u, 256u}) {
      const uint32_t dest = 0x50000, source = uint32_t(int32_t(dest) + shift);
      for (uint32_t i = 0; i < 2048; i += 16) FillVector(p, dest - 1024 + i, false);
      FillVector(p, 0x80000, false);
      FillVector(p, 0x80100, false);
      CHECK(Mix(p, Call(dest, source, 0x80000, 0x80100, n)));
    }
  }
}

// Declined, nothing written: buffers in the GPU's register window or wrapping, the stack slot inside the destination.
void Declines() {
  Pair p(9);
  FlushMode flush;
  if (!xdk::detail::HostHasFma()) {
    std::printf("xdk_xaudio_tests: this host has no FMA3: every call declined\n");
    CHECK(!Mix(p, Call(0x10000, 0x20000, 0x80000, 0x80100, 64)));
    return;
  }
  CHECK(!Mix(p, Call(0x7F000000, 0x20000, 0x80000, 0x80100, 64)));
  CHECK(!Mix(p, Call(0x10000, 0x7EFFFFF0, 0x80000, 0x80100, 64)));
  CHECK(!Mix(p, Call(0x10000, 0x20000, 0x7FC80000, 0x80100, 64)));
  CHECK(!Mix(p, Call(0xFFFFFFC0, 0x20000, 0x80000, 0x80100, 256)));
  CHECK(!Mix(p, Call(kStack - 32, 0x20000, 0x80000, 0x80100, 64)));
  // Fewer than four samples: nothing is mixed, but r31 is saved and the gains are loaded: accepted.
  CHECK(Mix(p, Call(0x7F000000, 0x7F000000, 0x80000, 0x80100, 3)));
}

// XAudio's fill against its model: the five sizes, blocks at any alignment, values of every kind (signalling NaNs
// and denormals keep their bits), a block past the end of plain memory.
void Fills() {
  Pair p(13);
  std::mt19937 rng(17);
  for (uint32_t rounds : {32u, 64u, 128u, 256u, 512u}) {
    for (int i = 0; i < 40; ++i) {
      const uint32_t block = 0x20000 + uint32_t(rng() % 4096) * (i % 3 == 0 ? 1 : 4);
      const uint32_t value = i % 5 == 0 ? 0x7F800001u : i % 5 == 1 ? 0x00000003u : uint32_t(rng());
      for (int b = 0; b < 4; ++b) p.native[block + 4 + b] = p.model[block + 4 + b] = uint8_t(value >> (8 * (3 - b)));
      uint32_t bits = 0;
      CHECK(xdk::XAudioFill(p.a, block, rounds * 8, bits));
      xdk::reference::XAudioFill(p.b, block, rounds);
      CHECK(p.SameEverywhere());
      CHECK(bits == value);
    }
  }
  uint32_t bits = 0;
  CHECK(!xdk::XAudioFill(p.a, 0x7EFFF000u, 4096, bits));
  CHECK(!xdk::XAudioFill(p.a, 0xFFFFF000u, 4096, bits));
  CHECK(p.SameEverywhere());
}

}  // namespace

int main() {
  Apart();
  SpecialValues();
  Overlaps();
  Declines();
  Fills();
  return xdk_test::Finish("xdk_xaudio_tests");
}
