// Unit tests of the D3D runtime's state setters (any360/xdk/d3d_state.h) against their instruction-by-instruction
// models (any360/xdk/d3d_reference.h): random devices, values, registers, counts and sources, in the default layout
// and in a shifted one. A call the replacement takes must leave the same bytes as the model; one it declines must
// leave memory as it was (the runtime's own code then runs; for a count of 0 it would loop for ever).
#include <cstdint>
#include <cstdio>
#include <random>

#include "any360/xdk/d3d_reference.h"
#include "any360/xdk/d3d_state.h"
#include "xdk_test.h"

namespace {

namespace xdk = any360::xdk;
namespace ref = any360::xdk::reference::state;
using xdk_test::kArena;
using xdk_test::Pair;

constexpr uint32_t kDevice = 0x20000;
constexpr uint32_t kStackPointer = xdk_test::kStack;

xdk::D3dLayout Shifted() {
  xdk::D3dLayout l;
  l.state_dirty += 40, l.constant_dirty += 40, l.blend_control += 40, l.blend_flags += 40;
  for (uint32_t& t : l.target_blend) t += 40;
  for (int i = 0; i < 2; ++i) l.bool_constants[i] += 40, l.int_constants[i] += 40, l.float_dirty[i] += 40;
  l.float_constant_base[0] += 3, l.float_constant_base[1] += 3;
  l.blend_extent += 40;
  l.fetch_constants += 40, l.vertex_fetch_last += 40, l.fetch_dirty += 40, l.texture_bound += 40;
  l.stream_bound += 40, l.texture_mip_floor += 40, l.texture_mip_ceiling += 40, l.stream_stride += 40;
  l.shader_stream_stride += 40, l.resource_fence += 40, l.resource_busy_mask += 40, l.release_cursor += 40;
  l.release_end += 40;
  return l;
}

void Put32Both(Pair& p, uint32_t at, uint32_t v) {
  ref::stw(p.a, at, v);
  ref::stw(p.b, at, v);
}

uint32_t RandomValue(std::mt19937_64& rng) {
  switch (rng() % 4) {
    case 0: return 0;
    case 1: return 1;
    case 2: return uint32_t(rng() % 32);
    default: return uint32_t(rng());
  }
}

void TestBlend(const xdk::D3dLayout& l, uint32_t seed) {
  std::mt19937_64 rng(seed);
  struct Field {
    xdk::D3dBlendField field;
    uint32_t sh, mb, me;
  };
  const Field fields[6] = {{xdk::D3dBlendField::kColorSource, 0, 27, 31},     {xdk::D3dBlendField::kColorOperation, 5, 24, 26},
                           {xdk::D3dBlendField::kColorDestination, 8, 19, 23}, {xdk::D3dBlendField::kAlphaSource, 16, 11, 15},
                           {xdk::D3dBlendField::kAlphaOperation, 21, 8, 10},   {xdk::D3dBlendField::kAlphaDestination, 24, 3, 7}};
  for (int round = 0; round < 400; ++round) {
    Pair p(seed + round);
    for (int call = 0; call < 16; ++call) {
      if (rng() % 4 == 0) Put32Both(p, kDevice + l.blend_flags, uint32_t(rng()));
      if (rng() % 4 == 0) Put32Both(p, kDevice + l.blend_control, uint32_t(rng()));
      const uint64_t r4 = rng() % 2 ? RandomValue(rng) : rng();
      const uint32_t kind = uint32_t(rng() % 8);
      bool taken;
      if (kind < 6) {
        const Field& f = fields[kind];
        taken = xdk::D3dSetBlendField(p.a, l, kDevice, f.field, uint32_t(r4));
        if (kind < 3) ref::BlendColorField(p.b, l, kDevice, r4, f.sh, f.mb, f.me);
        else ref::BlendAlphaField(p.b, l, kDevice, r4, f.sh, f.mb, f.me);
      } else if (kind == 6) {
        taken = xdk::D3dSetBlendEnable(p.a, l, kDevice, uint32_t(r4));
        ref::BlendEnable(p.b, l, kDevice, r4);
      } else {
        taken = xdk::D3dSetSeparateAlphaBlend(p.a, l, kDevice, uint32_t(r4));
        ref::SeparateAlphaBlend(p.b, l, kDevice, r4);
      }
      CHECK(taken);
      CHECK(p.Same(kDevice, kDevice + l.blend_extent));
    }
    CHECK(p.SameEverywhere());
  }
  // A device whose fields reach the GPU's register window is declined, memory untouched.
  Pair p(seed);
  CHECK(!xdk::D3dSetBlendEnable(p.a, l, 0x7F000000u - 64, 1));
}

// A source among the device's words now and then (the runtime's code would read what it has just written).
uint32_t RandomSource(std::mt19937_64& rng, uint32_t words_at) {
  if (rng() % 16 == 0) return words_at + uint32_t(rng() % 64) - 32;
  return 0x40000 + uint32_t(rng() % 0x10000);
}

void TestBoolConstants(const xdk::D3dLayout& l, uint32_t seed) {
  std::mt19937_64 rng(seed);
  uint64_t taken = 0, declined = 0;
  for (int round = 0; round < 3000; ++round) {
    Pair p(seed + round);
    const bool pixel = rng() % 2;
    const uint32_t first = rng() % 8 ? uint32_t(rng() % 256) : uint32_t(rng() % 4096);
    const uint32_t count = rng() % 16 == 0 ? 0 : 1 + uint32_t(rng() % (rng() % 4 ? 32 : 300));
    const uint32_t source = RandomSource(rng, kDevice + l.bool_constants[pixel] + 4 * (first >> 5));
    if (xdk::D3dSetBoolConstants(p.a, l, kDevice, pixel, first, source, count)) {
      ++taken;
      ref::BoolConstants(p.b, l, kDevice, pixel, first, source, count);
      CHECK(p.SameEverywhere());
    } else {
      ++declined;
      CHECK(p.SameEverywhere());
      CHECK(count == 0 || count > 256 || source < 0x40000);
    }
  }
  CHECK(taken > 2000 && declined > 50);
}

void TestIntConstants(const xdk::D3dLayout& l, uint32_t seed) {
  std::mt19937_64 rng(seed);
  uint64_t taken = 0, declined = 0;
  for (int round = 0; round < 3000; ++round) {
    Pair p(seed + round);
    const bool pixel = rng() % 2;
    const uint32_t first = rng() % 8 ? uint32_t(rng() % 32) : uint32_t(rng() % 4096);
    const uint32_t count = rng() % 16 == 0 ? 0 : 1 + uint32_t(rng() % (rng() % 4 ? 16 : 300));
    const uint32_t source = RandomSource(rng, kDevice + l.int_constants[pixel] + 4 * first);
    if (xdk::D3dSetIntConstants(p.a, l, kDevice, pixel, first, source, count)) {
      ++taken;
      ref::IntConstants(p.b, l, kDevice, pixel, first, source, count);
      CHECK(p.SameEverywhere());
    } else {
      ++declined;
      CHECK(p.SameEverywhere());
      CHECK(count == 0 || count > 256 || source < 0x40000);
    }
  }
  CHECK(taken > 2000 && declined > 50);
}

void TestPixelFloatConstants(const xdk::D3dLayout& l, uint32_t seed) {
  std::mt19937_64 rng(seed);
  uint64_t taken = 0;
  for (int round = 0; round < 2000; ++round) {
    Pair p(seed + round);
    const uint64_t first = rng() % 256;
    const uint32_t source = 0x40000 + uint32_t(rng() % 4096);
    const uint32_t count = rng() % 4 == 0 ? uint32_t(rng() % 64) : uint32_t(rng() % 9);
    const uint64_t dirty = rng() % 2 ? uint64_t(1) << (rng() % 64) : rng();
    if (xdk::D3dSetPixelShaderConstants(p.a, l, kDevice, first, source, count, dirty, kStackPointer)) {
      ++taken;
      any360::xdk::reference::D3dSetShaderConstants(p.b, kDevice, first, source, count, dirty, kStackPointer,
                                                    l.float_constant_base[1], l.float_dirty[1]);
    }
    CHECK(p.SameEverywhere());
  }
  CHECK(taken > 1900);
}

// The binders' common inputs: the old resource in the slot (none, a header in the arena, one inside the device: declined),
// a fence pending or not, the busy mask, the release list with room or full.
void RandomRelease(Pair& p, const xdk::D3dLayout& l, std::mt19937_64& rng, uint32_t slot) {
  uint32_t old = 0;
  switch (rng() % 6) {
    case 0: old = 0; break;
    case 1: old = kDevice + uint32_t(rng() % 1000); break;  // declined
    default: old = 0x50000 + uint32_t(rng() % 0x1000); break;
  }
  Put32Both(p, kDevice + slot, old);
  Put32Both(p, kDevice + l.resource_fence, rng() % 3 == 0 ? uint32_t(rng()) : 0);
  Put32Both(p, kDevice + l.resource_busy_mask, rng() % 4 == 0 ? 0 : uint32_t(rng()));
  const uint32_t cursor = 0x60000 + 8 * uint32_t(rng() % 256);
  Put32Both(p, kDevice + l.release_cursor, cursor);
  Put32Both(p, kDevice + l.release_end, rng() % 6 == 0 ? cursor : cursor + 64);
}

void TestSetTexture(const xdk::D3dLayout& l, uint32_t seed) {
  std::mt19937_64 rng(seed);
  uint64_t taken = 0, declined = 0;
  for (int round = 0; round < 3000; ++round) {
    Pair p(seed + round);
    const uint32_t stage = rng() % 16 ? uint32_t(rng() % 32) : 32 + uint32_t(rng() % 8);
    const uint32_t texture = rng() % 4 == 0 ? 0 : rng() % 32 == 0 ? kDevice + 64 : 0x40000 + uint32_t(rng() % 0x10000);
    const uint64_t dirty = rng() % 2 ? uint64_t(1) << (rng() % 64) : rng();
    RandomRelease(p, l, rng, l.texture_bound + 4 * (stage % 32));
    if (xdk::D3dSetTexture(p.a, l, kDevice, stage, texture, dirty, kStackPointer)) {
      ++taken;
      ref::SetTexture(p.b, l, kDevice, stage, texture, dirty, kStackPointer);
      CHECK(p.SameEverywhere());
    } else {
      ++declined;
      CHECK(p.SameEverywhere());
    }
  }
  CHECK(taken > 1500 && declined > 100);
}

void TestSetStreamSource(const xdk::D3dLayout& l, uint32_t seed) {
  std::mt19937_64 rng(seed);
  uint64_t taken = 0, declined = 0;
  for (int round = 0; round < 3000; ++round) {
    Pair p(seed + round);
    const uint32_t stream = rng() % 16 ? uint32_t(rng() % 16) : 16 + uint32_t(rng() % 8);
    const uint32_t buffer = rng() % 4 == 0 ? 0 : rng() % 32 == 0 ? kDevice + 64 : 0x40000 + uint32_t(rng() % 0x10000);
    const uint32_t offset = rng() % 3 == 0 ? 0 : uint32_t(rng() % 0x10000);
    const uint32_t stride = rng() % 8 == 0 ? uint32_t(rng()) : 4 * uint32_t(rng() % 32);
    const uint64_t dirty = rng() % 2 ? uint64_t(1) << (rng() % 64) : rng();
    if (rng() % 2) p.a.base[kDevice + l.shader_stream_stride + stream % 16] = p.b.base[kDevice + l.shader_stream_stride + stream % 16] = uint8_t(stride >> 2);
    RandomRelease(p, l, rng, l.stream_bound + 4 * (stream % 16));
    if (xdk::D3dSetStreamSource(p.a, l, kDevice, stream, buffer, offset, stride, dirty, kStackPointer)) {
      ++taken;
      ref::SetStreamSource(p.b, l, kDevice, stream, buffer, offset, stride, dirty, kStackPointer);
      CHECK(p.SameEverywhere());
    } else {
      ++declined;
      CHECK(p.SameEverywhere());
    }
  }
  CHECK(taken > 1500 && declined > 100);
}

}  // namespace

int main() {
  const xdk::D3dLayout standard, shifted = Shifted();
  TestBlend(standard, 100);
  TestBlend(shifted, 200);
  TestBoolConstants(standard, 300);
  TestBoolConstants(shifted, 400);
  TestIntConstants(standard, 500);
  TestIntConstants(shifted, 600);
  TestPixelFloatConstants(standard, 700);
  TestPixelFloatConstants(shifted, 800);
  TestSetTexture(standard, 900);
  TestSetTexture(shifted, 1000);
  TestSetStreamSource(standard, 1100);
  TestSetStreamSource(shifted, 1200);
  return xdk_test::Finish("xdk_d3d_state_tests");
}
