// What xdk_xaudio_reverb_tests and xdk_xaudio_reverb_bench share: the guest memory (a sparse reservation of the
// 32-bit space, so that the few constants the effect reads from its build's image sit at their addresses), where the
// state, stack and channels go, and states made from scratch (no title's data).
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "any360/xdk/xaudio_reverb.h"

namespace xaudio_reverb_test {

namespace xdk = any360::xdk;

// The 32-bit guest space reserved, committed where the test puts things.
struct SparseGuest {
  uint8_t* base = nullptr;
  SparseGuest() {
#if defined(_WIN32)
    base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS));
#else
    void* p = mmap(nullptr, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    base = p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
  }
  ~SparseGuest() {
#if defined(_WIN32)
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, 0x100000000ull);
#endif
  }
  void Commit(uint32_t at, uint32_t size) {
    const uint32_t begin = at & ~0xFFFu, end = (at + size + 0xFFF) & ~0xFFFu;
#if defined(_WIN32)
    VirtualAlloc(base + begin, end - begin, MEM_COMMIT, PAGE_READWRITE);
#else
    mprotect(base + begin, end - begin, PROT_READ | PROT_WRITE);
#endif
  }
  xdk::Memory Memory() const {
    xdk::Memory m;
    m.base = base;
    return m;
  }
};

// Where the test puts things: the state, the stack, the channels' buffers and pointers, and the constants' pages.
constexpr uint32_t kState = 0x100000, kStack = 0x200000, kInputs = 0x300000, kOutputs = 0x310000, kPointers = 0x320000;
constexpr uint32_t kWorkBegin = 0x100000, kWorkEnd = 0x330000;
struct Constant {
  uint32_t at;
  float value;
};
// The few image constants the effect reads (Crackdown's addresses; values of the same kind).
constexpr Constant kConstants[] = {{0x82000A8C, 2.0f},       {0x82000A90, 1.0f},  {0x82000A94, 0.0f},
                                   {0x82001C00, 10000.0f},   {0x8200247C, 8.0f},  {0x8205597C, 0.0512f},
                                   {0x820EDD7C, 0.0002f},    {0x82BA2F80, 1.0f},  {0x82BA2F84, 1.0f},
                                   {0x82BA2F88, 1.0f},       {0x82BA2F8C, 1.0f}};

// The state's integer words (positions, delays, crossfade taps): every other word is a float.
constexpr uint32_t kIntegers[] = {
    0x00064, 0x00070, 0x00074, 0x00078, 0x00080, 0x10090, 0x10094, 0x10098, 0x100A0, 0x200F0, 0x200FC, 0x20108,
    0x20114, 0x2091C, 0x2092C, 0x20B34, 0x20B40, 0x20B4C, 0x20B58, 0x22B60, 0x22B70, 0x22F78, 0x22F88, 0x23790,
    0x2379C, 0x237A8, 0x247B8, 0x247C4, 0x247D0, 0x247DC, 0x24FE4, 0x24FF4, 0x251FC, 0x25208, 0x25214, 0x25220,
    0x27228, 0x27238, 0x27640, 0x27650, 0x27E58, 0x27E64, 0x27E70, 0x28E78, 0x28E88, 0x29690, 0x296A0, 0x29EA8,
    0x29EC0, 0x29EE0, 0x29EFC, 0x29F08, 0x29F14, 0x2DF1C, 0x2DF34, 0x2DF44, 0x2E74C, 0x2E75C, 0x2EF64, 0x2EF7C,
    0x2EF9C, 0x2EFB8, 0x2EFC4, 0x2EFD0, 0x32FD8, 0x32FF0, 0x33000, 0x35008, 0x3501C, 0x37024, 0x37034, 0x3903C,
    0x3904C, 0x3A054, 0x3A060, 0x3A06C, 0x3E074, 0x3E090, 0x3E0A0, 0x400A8, 0x400BC, 0x420C4, 0x420D4, 0x440DC,
    0x440EC, 0x450F4, 0x45100, 0x4510C, 0x49114, 0x49130, 0x49140, 0x49948, 0x49958, 0x4A160, 0x4A170, 0x4A978,
    0x4A988, 0x4B198, 0x4B19C, 0x4B1A8, 0x4B5B8, 0x4B5BC, 0x4B5C0, 0x4B5C8};
// Read-only delays (taps): any value is read through a mask; the test keeps them below 2048.
constexpr uint32_t kTaps[] = {0x200F0, 0x200FC, 0x20108, 0x2091C, 0x20B34, 0x20B40, 0x20B4C, 0x22B60, 0x22F78,
                              0x23790, 0x2379C, 0x247B8, 0x247C4, 0x247D0, 0x24FE4, 0x251FC, 0x25208, 0x25214,
                              0x27228, 0x27640, 0x27E58, 0x27E64, 0x28E78, 0x29690, 0x2DF1C, 0x2DF34, 0x2E74C,
                              0x32FD8, 0x32FF0, 0x35008, 0x37024, 0x3903C, 0x3A054, 0x3A060, 0x3E074, 0x3E090,
                              0x400A8, 0x420C4, 0x440DC, 0x450F4, 0x45100, 0x49130, 0x49948, 0x4A160, 0x4A978};
// The crossfading delay lines: their fraction (a float) and their target tap.
constexpr uint32_t kFractions[] = {0x0006C, 0x1008C, 0x4B194, 0x4B5B4};
constexpr uint32_t kTargets[] = {0x00078, 0x10098, 0x4B1A0, 0x4B5C0};

inline void Put32(const xdk::Memory& m, uint32_t at, uint32_t v) { xdk::ppc_model::Store32(m, at, v); }
inline void PutF(const xdk::Memory& m, uint32_t at, float v) { Put32(m, at, std::bit_cast<uint32_t>(v)); }

struct Side {
  SparseGuest guest;
  xdk::Memory m;
  Side() {
    guest.Commit(kWorkBegin, kWorkEnd - kWorkBegin);
    for (const Constant& c : kConstants) guest.Commit(c.at, 4);
    m = guest.Memory();
    for (const Constant& c : kConstants) PutF(m, c.at, c.value);
  }
  uint8_t* At(uint32_t a) const { return m.Host(a); }
};

// Lays out a fresh state and the channels on both sides (the same bytes). `scale` multiplies its floats (delay lines
// and coefficients): a small one makes a reverb dying out, every product in the denormal range.
inline void NewState(Side& a, Side& b, std::mt19937& rng, uint32_t mode, bool crossfade, float scale = 1.0f) {
  std::uniform_real_distribution<float> sample(-1.0f, 1.0f), coefficient(-0.9f, 0.9f), fraction(0.0f, 0.06f);
  std::vector<uint8_t> state(xdk::kXAudioReverbStateSize, 0);
  for (uint32_t at = 0; at < state.size(); at += 4) {
    const uint32_t v = std::byteswap(std::bit_cast<uint32_t>(scale * coefficient(rng)));
    std::memcpy(&state[at], &v, 4);
  }
  const auto put = [&](uint32_t at, uint32_t v) {
    v = std::byteswap(v);
    std::memcpy(&state[at], &v, 4);
  };
  for (uint32_t at : kIntegers) put(at, 0);
  for (uint32_t at : kTaps) put(at, rng() % 2048);
  for (uint32_t at : kFractions) put(at, std::bit_cast<uint32_t>(rng() % 4 ? fraction(rng) : coefficient(rng)));
  if (crossfade) {
    for (uint32_t at : kTargets) put(at, rng() % 4096);
  }
  put(100, mode);
  for (Side* s : {&a, &b}) {
    std::memcpy(s->At(kState), state.data(), state.size());
    Put32(s->m, kPointers + 0, kInputs);
    Put32(s->m, kPointers + 4, kInputs + 0x1000);
    for (uint32_t c = 0; c < 6; ++c) Put32(s->m, kPointers + 0x40 + 4 * c, kOutputs + 0x1000 * c);
  }
}

// The two input channels' samples; `scale` 0 gives silence (what a reverb hears once a sound has ended).
inline void NewInputs(Side& a, Side& b, std::mt19937& rng, float scale = 1.0f) {
  std::uniform_real_distribution<float> sample(-1.0f, 1.0f);
  for (uint32_t i = 0; i < 512; ++i) {
    const uint32_t at = kInputs + (i < 256 ? 4 * i : 0x1000 + 4 * (i - 256));
    const float v = rng() % 64 ? scale * sample(rng) : 0.0f;
    PutF(a.m, at, v);
    PutF(b.m, at, v);
  }
}

}  // namespace xaudio_reverb_test
