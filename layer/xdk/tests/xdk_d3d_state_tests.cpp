// Unit tests of the D3D runtime's state setters (any360/xdk/d3d_state.h) against their instruction-by-instruction
// models (any360/xdk/d3d_reference.h): random devices, values, registers, counts and sources, in the default layout
// and in a shifted one. A call the replacement takes must leave the same bytes as the model; one it declines must
// leave memory as it was (the runtime's own code then runs; for a count of 0 it would loop for ever).
#include <cmath>
#include <limits>
#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "any360/xdk/d3d_reference.h"
#include "any360/xdk/d3d_state.h"
#include "any360/xdk/d3d_tiling.h"
#include "any360/sys/alloc_meter.h"
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
  l.stage_filter_index += 40, l.stage_lod_limit += 40, l.vertex_declaration += 40, l.pixel_shader += 40;
  l.vertex_shader += 40, l.shader_flags += 40;
  l.clip_control += 40, l.mode_control += 40, l.vte_control += 40, l.poly_offset += 40;
  l.surfaces += 40, l.tiled_surfaces += 40, l.surface_info += 40, l.color_info += 40, l.color_mask += 40;
  l.depth_control += 40, l.depth_word += 40, l.color_mask_set += 40, l.depth_enable_set += 40;
  l.stencil_enable_set += 40, l.target_as_wide += 40, l.bin_mask_set += 40, l.tiling_flags += 40;
  l.depth_flags += 40, l.tiling_options += 40, l.tiling_pending[0] += 40, l.tiling_pending[1] += 40;
  l.tiling_hiz_pitch += 40;
  l.color_control += 40, l.blend_color += 40, l.clip_plane_flag += 40, l.index_buffer += 40;
  l.clip_planes += 48;  // the runtime finds a plane as (index + clip_planes / 16) << 4: kept on 16 bytes
  l.viewport_set += 40, l.scissor_set += 40, l.window_scissor += 40, l.viewport_scale += 40, l.scissor_enable += 40;
  l.tiled_size += 40, l.tile_count += 40, l.tile_rects += 40, l.tile_offsets += 40, l.tile_bins += 40;
  l.sample_control += 40;
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

// The sampler filters: the runtime's table placed in the arena (its address is the title's).
void TestSamplerFilter(xdk::D3dLayout l, uint32_t seed) {
  l.sampler_filter_table = 0x70000;
  std::mt19937_64 rng(seed);
  uint64_t taken = 0;
  for (int round = 0; round < 3000; ++round) {
    Pair p(seed + round);
    const uint32_t stage = rng() % 16 ? uint32_t(rng() % 32) : 32 + uint32_t(rng() % 8);
    const uint64_t filter = rng() % 2 ? rng() % 8 : rng();
    const bool mag = rng() % 2;
    if (xdk::D3dSetSamplerFilter(p.a, l, kDevice, stage, uint32_t(filter), mag)) {
      ++taken;
      ref::SamplerFilter(p.b, l, kDevice, stage, filter, mag);
    }
    CHECK(p.SameEverywhere());
  }
  CHECK(taken > 2500);
}

void TestSetVertexDeclaration(const xdk::D3dLayout& l, uint32_t seed) {
  std::mt19937_64 rng(seed);
  uint64_t taken = 0, declined = 0;
  for (int round = 0; round < 2000; ++round) {
    Pair p(seed + round);
    RandomRelease(p, l, rng, l.vertex_declaration);
    const uint32_t declaration = rng() % 4 == 0 ? 0 : uint32_t(rng());
    if (xdk::D3dSetVertexDeclaration(p.a, l, kDevice, declaration, kStackPointer)) {
      ++taken;
      ref::SetVertexDeclaration(p.b, l, kDevice, declaration, kStackPointer);
    } else {
      ++declined;
    }
    CHECK(p.SameEverywhere());
  }
  CHECK(taken > 1200 && declined > 100);
}

// A shader with a constant program: skipped entries, copies, masked writes, each part ended or cut by the program's
// end; now and then an odd count (the runtime would loop for ever) or a place past the device (declined).
void PutHalf(Pair& p, uint32_t at, uint32_t v) {
  for (std::vector<uint8_t>* mem : {&p.native, &p.model}) (*mem)[at] = uint8_t(v >> 8), (*mem)[at + 1] = uint8_t(v);
}
uint32_t RandomShader(Pair& p, const xdk::D3dLayout& l, std::mt19937_64& rng, bool vertex) {
  const uint32_t shader = 0x40000 + 4 * uint32_t(rng() % 0x1000);
  const uint32_t block = shader + (vertex ? 872 : 40);
  if (rng() % 6 == 0) {
    Put32Both(p, block + 20, 0);
    return shader;
  }
  const uint32_t header = 0x50000 + 4 * uint32_t(rng() % 0x1000);
  Put32Both(p, block + 20, header - block);
  if (rng() % 3 == 0) Put32Both(p, header + 8, 0), Put32Both(p, header + 12, 0);
  const uint32_t program = header + 20;
  uint32_t at = program;
  const uint32_t room = l.device_bytes - l.fetch_constants;
  const auto place = [&](uint32_t bytes) { return rng() % 64 == 0 ? room : uint32_t(rng() % (768 - bytes)) & ~3u; };
  for (uint32_t i = rng() % 4; i > 0; --i) PutHalf(p, at + 2, 1 + uint32_t(rng() % 100)), at += 8;
  const bool cut = rng() % 5 == 0;
  if (!cut) {
    PutHalf(p, at + 2, 0), at += 4;
    for (uint32_t i = rng() % 4; i > 0; --i) {
      const uint32_t words = 1 + uint32_t(rng() % 8);
      PutHalf(p, at, place(4 * words)), PutHalf(p, at + 2, words), at += 4 + 4 * words;
    }
    if (rng() % 3) {
      PutHalf(p, at + 2, 0), at += 4;
      for (uint32_t i = rng() % 4; i > 0; --i) {
        const uint32_t count = rng() % 32 == 0 ? 3 : 2 * (1 + uint32_t(rng() % 8));
        PutHalf(p, at, place(2 * count)), PutHalf(p, at + 2, count), at += 4 + 4 * count;
      }
      if (rng() % 2) PutHalf(p, at + 2, 0), at += 4;
    }
  }
  Put32Both(p, header + 16, at - program);
  return shader;
}

void TestSetShader(const xdk::D3dLayout& l, uint32_t seed) {
  std::mt19937_64 rng(seed);
  uint64_t taken = 0, declined = 0;
  for (int round = 0; round < 4000; ++round) {
    Pair p(seed + round);
    const bool vertex = rng() % 2;
    RandomRelease(p, l, rng, vertex ? l.vertex_shader : l.pixel_shader);
    const uint32_t shader = rng() % 5 == 0 ? 0 : RandomShader(p, l, rng, vertex);
    if (xdk::D3dSetShader(p.a, l, kDevice, shader, vertex, kStackPointer)) {
      ++taken;
      ref::SetShader(p.b, l, kDevice, shader, vertex, kStackPointer);
    } else {
      ++declined;
    }
    CHECK(p.SameEverywhere());
  }
  CHECK(taken > 2500 && declined > 100);
  CHECK(xdk::D3dFlushCpuRange());
}

// The tile ops interpreter (any360/xdk/d3d_tiling.h): a bracket of two tiles (each segment submitted once a tile, each
// tile's select before its segments, the resources walked under the first tile's select only), a predicated jump not
// taken, an op read again until written, the run's end; and a list ended by 0xC0000000.
struct TileCalls : xdk::TileOpsCalls {
  const xdk::Memory& m;
  std::vector<std::string> log;
  uint32_t again_at = 0, agains = 0;
  explicit TileCalls(const xdk::Memory& memory) : m(memory) {}
  uint32_t Word(uint32_t a) const { return xdk::detail::At32(m.Host(a)); }
  void Note(const char* what, uint32_t a, uint32_t b) {
    char line[64];
    std::snprintf(line, sizeof line, "%s %X %X", what, a, b);
    log.push_back(line);
  }
  void Select(uint32_t state) override {
    const uint32_t mask = xdk::TileMask(Word(state + 40), Word(state + 44));
    if (mask == Word(state + 364)) return;
    xdk::detail::Set32(m.Host(state + 364), mask);
    Note("select", mask, 0);
  }
  void Submit(uint32_t dwords, uint32_t address) override { Note("submit", dwords, address); }
  void Resources(uint32_t header, uint32_t list, bool flush) override { Note("resources", list, flush ? 1 : 0); (void)header; }
  void Wait(uint32_t state) override { Note("wait", state, 0); }
  void LockL2() override { Note("lock", 0, 0); }
  void UnlockL2() override { Note("unlock", 0, 0); }
  void Again() override {
    if (++agains == 3) xdk::detail::Set32(m.Host(again_at), 0x82000007u);  // written now: a segment of 7 dwords
  }
};

// The allocation meter (any360/sys/alloc_meter.h) the benches and the self-test report with: a vector's buffer is
// counted on its thread, nothing on another thread's, and RunTileOps itself allocates nothing.
void TestAllocMeter() {
  const any360::sys::AllocCounts before = any360::sys::ThreadAllocations();
  {
    std::vector<uint32_t> v(1000);
    v[999] = 1;
    CHECK(v[999] == 1);
  }
  const any360::sys::AllocCounts spent = any360::sys::ThreadAllocations() - before;
  CHECK(spent.bytes >= 4000 && spent.count >= 1);
  const any360::sys::AllocCounts quiet = any360::sys::ThreadAllocations();
  CHECK((any360::sys::ThreadAllocations() - quiet).count == 0);
}

void TestTileOps() {
  Pair p(77);
  const xdk::Memory& m = p.a;
  constexpr uint32_t kState = 0x30000, kOps = 0x40000, kList = 0x50000;
  const auto put = [&](uint32_t a, uint32_t v) { xdk::detail::Set32(m.Host(a), v); };
  for (uint32_t a = kState; a < kState + 0x200; a += 4) put(a, 0);
  uint32_t at = kOps;
  const auto op = [&](std::initializer_list<uint32_t> words) {
    for (uint32_t w : words) put(at, w), at += 4;
  };
  op({0x80000000u});
  put(at, 2), put(at + 4, 2);  // the header: flags (bit 1), 2 tiles
  for (uint32_t i = 2; i < 62; ++i) put(at + 4 * i, 0);
  at += 248;
  op({0x87000000u, 0x82000003u, 0xA000, 0x8B000000u, 0x11111, kList, 0x8C000000u, 0x40000000u, 0x82000005u, 0xB000});
  const uint32_t again = at;
  op({0x83000000u, 0x0000C000u, 0x88000000u, 0x89000000u, 0x8A000000u, 0x81000000u});
  put(kList, kList + 12), put(kList + 4, 0x22222), put(kList + 8, 0x33333), put(kList + 12, 0xC0000000u);
  TileCalls calls(m);
  calls.again_at = again;
  const uint32_t stop = xdk::RunTileOps(m, kOps, kState, calls);
  const std::vector<std::string> want = {
      "select 80000003 0", "submit 3 A000", "resources 11111 0", "resources 22222 1", "resources 33333 1",
      "submit 5 B000",     "submit 7 C000", "select C 0",        "submit 3 A000",     "submit 5 B000",
      "submit 7 C000",     "select FFFFFFFF 0", "lock 0 0", "wait 30000 0", "unlock 0 0"};
  CHECK(calls.log == want);
  // The same run with calls that only count: the interpreter itself allocates nothing.
  struct Counting : xdk::TileOpsCalls {
    const xdk::Memory* m = nullptr;
    uint32_t n = 0;
    void Select(uint32_t state) override {  // the select kept in force, as TileSelect keeps it
      xdk::detail::Set32(m->Host(state + 364), xdk::TileMask(xdk::detail::At32(m->Host(state + 40)),
                                                             xdk::detail::At32(m->Host(state + 44))));
      ++n;
    }
    void Submit(uint32_t, uint32_t) override { ++n; }
    void Resources(uint32_t, uint32_t, bool) override { ++n; }
    void Wait(uint32_t) override { ++n; }
    void LockL2() override { ++n; }
    void UnlockL2() override { ++n; }
    void Again() override {}
  } counting;
  counting.m = &m;
  for (uint32_t a = kState; a < kState + 0x200; a += 4) put(a, 0);
  const any360::sys::AllocCounts run_start = any360::sys::ThreadAllocations();
  xdk::RunTileOps(m, kOps, kState, counting);
  const any360::sys::AllocCounts run_spent = any360::sys::ThreadAllocations() - run_start;
  CHECK(counting.n >= 13 && run_spent.count == 0 && run_spent.bytes == 0);
  CHECK(stop == at - 4 && calls.agains == 3);
  CHECK(xdk::detail::At32(m.Host(kState + 44)) == 2 && xdk::detail::At32(m.Host(kState + 376)) == kOps + 252);
  // A list's end.
  put(kOps, 0xC0000000u);
  TileCalls end(m);
  CHECK(xdk::RunTileOps(m, kOps, kState, end) == kOps && end.log.empty());
  // The masks: the even and odd tiles' halves, one tile alone, past 16 tiles nothing.
  CHECK(xdk::TileMask(0, 0) == 0xFFFFFFFFu && xdk::TileMask(0x80000000u, 3) == 0x2AAAAAAAu);
  CHECK(xdk::TileMask(0x40000000u, 3) == 0x15555555u && xdk::TileMask(0x20000000u, 2) == 0x30u);
  CHECK(xdk::TileMask(0x20000000u, 0) == 0x80000003u && xdk::TileMask(0xA0000000u, 0) == 0x2u);
  CHECK(xdk::TileMask(0x20000000u, 16) == 0);
}

}  // namespace

// SetRenderState_ViewportEnable and SetRenderState_DepthBias against their code read line by line: values as titles
// pass them and any 32 bits; the depth bias as float bits (0, -0, small values, NaN, denormals, any bits) with the
// scales 0 or not; the device's words and dirty words at random. The 0.0f the runtime compares with sits in the arena.
float RandomFloat(std::mt19937_64& rng) {
  static constexpr uint32_t kBits[] = {0x00000000u, 0x80000000u, 0x3F800000u, 0xBF000000u, 0x7FC00000u,
                                       0x7F800001u, 0x00000001u, 0x80000001u, 0x7F800000u, 0x3A83126Fu};
  return std::bit_cast<float>(rng() % 3 ? kBits[rng() % 10] : uint32_t(rng()));
}
void TestRenderStates(xdk::D3dLayout l, uint32_t seed) {
  std::mt19937_64 rng(seed);
  l.zero_float = 0x50000;
  for (int round = 0; round < 200; ++round) {
    Pair p(seed + round);
    Put32Both(p, l.zero_float, rng() % 8 ? 0u : std::bit_cast<uint32_t>(RandomFloat(rng)));
    for (int call = 0; call < 32; ++call) {
      if (rng() % 3 == 0) Put32Both(p, kDevice + l.poly_offset, std::bit_cast<uint32_t>(RandomFloat(rng)));
      if (rng() % 3 == 0) Put32Both(p, kDevice + l.poly_offset + 8, std::bit_cast<uint32_t>(RandomFloat(rng)));
      if (rng() % 2) {
        const uint64_t r4 = rng() % 2 ? RandomValue(rng) : rng();
        CHECK(xdk::D3dSetViewportEnable(p.a, l, kDevice, uint32_t(r4)));
        ref::ViewportEnable(p.b, l, kDevice, r4);
      } else {
        const uint32_t bits = std::bit_cast<uint32_t>(RandomFloat(rng));
        const uint64_t r4 = (rng() % 4 == 0 ? rng() << 32 : 0) | bits;
        CHECK(xdk::D3dSetDepthBias(p.a, l, kDevice, uint32_t(r4), kStackPointer + 28));
        ref::DepthBias(p.b, l, kDevice, r4, kStackPointer);
      }
      CHECK(p.Same(kDevice, kDevice + l.poly_offset + 16));
    }
    CHECK(p.SameEverywhere());
  }
  // Declined, memory untouched: the device reaching the GPU's register window, the parameter area over the device.
  Pair p(seed), q(seed);
  CHECK(!xdk::D3dSetViewportEnable(p.a, l, 0x7F000000u - 64, 1));
  CHECK(!xdk::D3dSetDepthBias(p.a, l, kDevice, 0x3F800000u, kDevice + l.poly_offset + 4));
  CHECK(p.native == q.native);
}

// SetRenderTarget and SetDepthStencilSurface against their code read line by line. The runtime's viewport function
// is a stand-in that writes what the later steps read (SURFACE_INFO, the dirty bits, the tiling flags, the bin mask's
// word), so a call made at another point shows. Surfaces bound or not, recorded or not, every tiling flag, formats
// 2_10_10_10 or not with their _AS_ bit as the target's word says or not (that word 0, 1 or any), indices past 3 and
// surfaces over the device declined.
void TestSurfaceSetters(const xdk::D3dLayout& l, uint32_t seed) {
  std::mt19937_64 rng(seed);
  constexpr uint32_t kSurfaces = 0x60000;
  auto stand_in = [&](const xdk::Memory& m, std::vector<uint32_t>& calls) {
    return [&m, &l, &calls](uint32_t surface) {
      calls.push_back(surface);
      ref::stw(m, kDevice + l.surface_info, surface ^ 0x5A5A1234u);
      ref::std_(m, kDevice + l.state_dirty, ref::ld(m, kDevice + l.state_dirty) | (uint64_t(1) << 57));
      *m.Host(kDevice + l.tiling_flags) ^= uint8_t(surface & 0x38);
      if (surface & 0x40) ref::stw(m, kDevice + l.bin_mask_set, 0);
    };
  };
  auto surface_at = [&](uint32_t k) { return kSurfaces + 64 * k; };
  for (int round = 0; round < 200; ++round) {
    Pair p(seed + round);
    for (int call = 0; call < 32; ++call) {
      for (uint32_t k = 0; k < 5; ++k) {
        if (rng() % 3 == 0) Put32Both(p, kDevice + l.surfaces + 4 * k, rng() % 3 ? surface_at(uint32_t(rng() % 8)) : 0);
        if (rng() % 3 == 0) {
          const uint32_t bound = ref::lwz(p.a, kDevice + l.surfaces + 4 * k);
          Put32Both(p, kDevice + l.tiled_surfaces + 4 * k, rng() % 2 ? bound : rng() % 2 ? 0 : surface_at(uint32_t(rng() % 8)));
        }
      }
      for (uint32_t k = 0; k < 4; ++k) {
        if (rng() % 4 == 0) Put32Both(p, kDevice + l.target_as_wide + 4 * k, rng() % 4 ? uint32_t(rng() % 2) : uint32_t(rng()));
      }
      if (rng() % 3 == 0) Put32Both(p, kDevice + l.bin_mask_set, rng() % 2 ? 0 : uint32_t(rng()));
      for (uint32_t at : {l.tiling_flags, l.tiling_options, l.tiling_pending[0], l.tiling_pending[1]}) {
        if (rng() % 3 == 0) *p.a.Host(kDevice + at) = *p.b.Host(kDevice + at) = rng() % 3 ? uint8_t(rng()) : 0;
      }
      for (uint32_t k = 0; k < 8; ++k) {
        if (rng() % 6) continue;
        static constexpr uint32_t kFormats[] = {2, 3, 10, 12, 0, 1, 6, 15};
        const uint32_t word = (uint32_t(rng()) & ~0xF0000u) | kFormats[rng() % 8] << 16;
        Put32Both(p, surface_at(k) + 28, word);
      }
      std::vector<uint32_t> calls_a, calls_b;
      const uint32_t choice = uint32_t(rng() % 8);
      const uint32_t surface = choice < 5 ? surface_at(uint32_t(rng() % 8)) : choice < 7 ? 0 : uint32_t(rng() % (kArena - 64)) & ~3u;
      const bool over_device = surface && surface + 40 > kDevice && surface < kDevice + l.device_bytes;
      if (rng() % 2) {
        const uint64_t r4 = rng() % 8 ? rng() % 4 : rng() % 16;
        const bool taken = xdk::D3dSetRenderTarget(p.a, l, kDevice, uint32_t(r4), surface, stand_in(p.a, calls_a));
        CHECK(taken == (r4 <= 3 && !over_device));
        if (taken) ref::SetRenderTarget(p.b, l, kDevice, r4, surface, stand_in(p.b, calls_b));
      } else {
        const bool taken = xdk::D3dSetDepthStencilSurface(p.a, l, kDevice, surface, stand_in(p.a, calls_a));
        CHECK(taken == !over_device);
        if (taken) ref::SetDepthStencilSurface(p.b, l, kDevice, surface, stand_in(p.b, calls_b));
      }
      CHECK(calls_a == calls_b);
      CHECK(p.SameEverywhere());
      if (!p.SameEverywhere()) {
        std::memcpy(p.model.data(), p.native.data(), kArena);  // one failure reported once
      }
    }
  }
}

// The render state fields' setters and getters and the sampler state siblings against their code read line by line:
// values as titles pass them and any 64 bits; the device's mirror words, polygon offsets, blend floats (0, -0, NaN,
// infinities, denormals, any bits), depth surface, fetch constants and stage bytes at random; the runtime's float
// constants (their values and any) in the arena. Each call compared over the device; the round's memory everywhere.
void TestRenderStateFields(xdk::D3dLayout l, uint32_t seed) {
  std::mt19937_64 rng(seed);
  l.zero_float = 0x50000, l.blend_byte_scale = 0x50010, l.blend_float_scale = 0x50014, l.blend_rounding = 0x50018;
  l.slope_scale_factor = 0x5001C, l.sampler_filter_table = 0x70000;
  auto value = [&]() -> uint64_t { return rng() % 4 == 0 ? rng() : RandomValue(rng); };
  auto float_bits = [&]() { return std::bit_cast<uint32_t>(RandomFloat(rng)); };
  uint64_t taken = 0;
  for (int round = 0; round < 300; ++round) {
    Pair p(seed + round);
    Put32Both(p, l.zero_float, rng() % 8 ? 0u : float_bits());
    Put32Both(p, l.blend_byte_scale, rng() % 4 ? std::bit_cast<uint32_t>(1.0f / 255.0f) : float_bits());
    Put32Both(p, l.blend_float_scale, rng() % 4 ? std::bit_cast<uint32_t>(255.0f) : float_bits());
    Put32Both(p, l.blend_rounding, rng() % 4 ? std::bit_cast<uint32_t>(0.5f) : float_bits());
    Put32Both(p, l.slope_scale_factor, rng() % 4 ? std::bit_cast<uint32_t>(16.0f) : float_bits());
    for (int call = 0; call < 48; ++call) {
      for (uint32_t k = 0; k < 4; ++k) {
        if (rng() % 4 == 0) Put32Both(p, kDevice + l.blend_color + 4 * k, rng() % 2 ? float_bits() : std::bit_cast<uint32_t>(float(rng() % 256) / 255.0f));
        if (rng() % 6 == 0) Put32Both(p, kDevice + l.poly_offset + 4 * k, rng() % 2 ? 0u : float_bits());
      }
      if (rng() % 4 == 0) Put32Both(p, kDevice + l.surfaces + 16, rng() % 2 ? 0u : uint32_t(rng()));
      const uint32_t stage = rng() % 16 ? uint32_t(rng() % 32) : 32 + uint32_t(rng() % 8);
      if (stage < 32) {
        if (rng() % 3 == 0) Put32Both(p, kDevice + l.fetch_constants + 24 * stage + 16, uint32_t(rng()));
        if (rng() % 3 == 0) Put32Both(p, kDevice + l.fetch_constants + 24 * stage + 12, uint32_t(rng()));
        if (rng() % 3 == 0) *p.a.Host(kDevice + l.stage_lod_limit + stage) = *p.b.Host(kDevice + l.stage_lod_limit + stage) = uint8_t(rng());
      }
      const uint64_t r4 = value();
      const uint32_t which = uint32_t(rng() % 21);
      bool expect = true, took = false;
      uint64_t mine = 0, theirs = 0;
      uint32_t got = 0;
      switch (which) {
        case 0: took = xdk::D3dSetCullMode(p.a, l, kDevice, uint32_t(r4)); ref::CullMode(p.b, l, kDevice, r4); break;
        case 1: took = xdk::D3dSetAlphaTestEnable(p.a, l, kDevice, uint32_t(r4)); ref::AlphaTestEnable(p.b, l, kDevice, r4); break;
        case 2: took = xdk::D3dSetAlphaFunc(p.a, l, kDevice, uint32_t(r4)); ref::AlphaFunc(p.b, l, kDevice, r4); break;
        case 3: took = xdk::D3dSetBlendFactor(p.a, l, kDevice, uint32_t(r4)); ref::BlendFactor(p.b, l, kDevice, r4); break;
        case 4: took = xdk::D3dSetZEnable(p.a, l, kDevice, uint32_t(r4)); ref::ZEnable(p.b, l, kDevice, r4); break;
        case 5: took = xdk::D3dSetZWriteEnable(p.a, l, kDevice, uint32_t(r4)); ref::ZWriteEnable(p.b, l, kDevice, r4); break;
        case 6: took = xdk::D3dSetClipPlaneEnable(p.a, l, kDevice, uint32_t(r4)); ref::ClipPlaneEnable(p.b, l, kDevice, r4); break;
        case 7: {
          const uint64_t bits = (rng() % 4 == 0 ? rng() << 32 : 0) | float_bits();
          took = xdk::D3dSetSlopeScaleDepthBias(p.a, l, kDevice, uint32_t(bits), kStackPointer + 28);
          ref::SlopeScaleDepthBias(p.b, l, kDevice, bits, kStackPointer);
          break;
        }
        case 8: took = xdk::D3dSetIndices(p.a, l, kDevice, uint32_t(r4)); ref::SetIndices(p.b, l, kDevice, r4); break;
        case 9: {
          const uint64_t index = rng() % 8 ? rng() % 6 : rng() % 64;
          const uint32_t plane = rng() % 8 ? 0x60000 + uint32_t(rng() % 0x1000) : kDevice + uint32_t(rng() % 0x4000);
          expect = index < 6 && !(plane + 16 > kDevice && plane < kDevice + l.device_bytes);
          took = xdk::D3dSetClipPlane(p.a, l, kDevice, uint32_t(index), plane);
          if (took) ref::SetClipPlane(p.b, l, kDevice, index, plane);
          break;
        }
        case 10: case 11: case 12: case 13: case 14: case 15: case 16: {
          static constexpr xdk::D3dRenderStateRead kStates[] = {
              xdk::D3dRenderStateRead::kCullMode, xdk::D3dRenderStateRead::kAlphaTestEnable,
              xdk::D3dRenderStateRead::kBlendEnable, xdk::D3dRenderStateRead::kSrcBlend,
              xdk::D3dRenderStateRead::kDestBlend, xdk::D3dRenderStateRead::kAlphaFunc,
              xdk::D3dRenderStateRead::kZWriteEnable};
          using Get = uint64_t (*)(const xdk::Memory&, const xdk::D3dLayout&, uint32_t);
          static constexpr Get kRefs[] = {ref::GetCullMode, ref::GetAlphaTestEnable, ref::GetBlendEnable, ref::GetSrcBlend,
                                          ref::GetDestBlend, ref::GetAlphaFunc, ref::GetZWriteEnable};
          took = xdk::D3dGetRenderState(p.a, l, kDevice, kStates[which - 10], got);
          mine = got, theirs = kRefs[which - 10](p.b, l, kDevice);
          break;
        }
        case 17: took = xdk::D3dGetBlendFactor(p.a, l, kDevice, got); mine = got, theirs = ref::GetBlendFactor(p.b, l, kDevice); break;
        case 18: {
          const uint64_t index = rng() % 4 ? rng() % 8 : (rng() % 0x20000) | (rng() % 2 ? rng() << 32 : 0);
          expect = stage < 32;
          took = xdk::D3dSetSamplerAnisotropy(p.a, l, kDevice, stage, uint32_t(index));
          if (took) ref::SamplerAnisotropy(p.b, l, kDevice, stage, index);
          break;
        }
        default: {
          const uint32_t bit = which == 19 ? 1 : 0;
          expect = stage < 32;
          took = xdk::D3dSetSamplerLodFlag(p.a, l, kDevice, stage, uint32_t(r4), bit);
          if (took) ref::SamplerLodFlag(p.b, l, kDevice, stage, r4, bit);
          break;
        }
      }
      CHECK(took == expect);
      taken += took;
      CHECK(mine == theirs);
      CHECK(p.Same(kDevice, kDevice + l.device_bytes));
      if (mine != theirs || !p.Same(kDevice, kDevice + l.device_bytes)) {
        if (xdk_test::g_failures <= 40) {
          std::printf("  function %u, stage %u, r4 %llx\n", which, stage, static_cast<unsigned long long>(r4));
        }
        std::memcpy(p.model.data(), p.native.data(), kArena);  // one failure reported once
      }
    }
    CHECK(p.SameEverywhere());
    if (!p.SameEverywhere()) std::memcpy(p.model.data(), p.native.data(), kArena);
  }
  CHECK(taken > 13000);
  // Declined, memory untouched: a plane over the device, a stage past 31, the device in the GPU's register window.
  Pair p(seed), q(seed);
  CHECK(!xdk::D3dSetClipPlane(p.a, l, kDevice, 0, kDevice + 8));
  CHECK(!xdk::D3dSetClipPlane(p.a, l, kDevice, 6, 0x60000));
  CHECK(!xdk::D3dSetSamplerLodFlag(p.a, l, kDevice, 32, 1, 0));
  CHECK(!xdk::D3dSetCullMode(p.a, l, 0x7F000000u - 64, 1));
  CHECK(!xdk::D3dSetSlopeScaleDepthBias(p.a, l, kDevice, 0x3F800000u, kDevice + l.poly_offset + 4));
  CHECK(p.native == q.native);
}

// The window scissor's packets as a list of writes: register writes (type-0 packets, SET_CONSTANT's context registers,
// register records) and bin masks (SET_BIN_MASK_LO/HI, bins records), each with its predication, in the order written.
// PM4 and records of the same writes give the same list.
std::vector<std::array<uint32_t, 4>> ScissorCommands(const std::vector<uint8_t>& memory, uint32_t from, uint32_t to) {
  namespace record = any360::xdk::record;
  std::vector<std::array<uint32_t, 4>> out;
  const auto big = [&](uint32_t a) {
    return uint32_t(memory[a]) << 24 | uint32_t(memory[a + 1]) << 16 | uint32_t(memory[a + 2]) << 8 | memory[a + 3];
  };
  const auto host = [&](uint32_t a) { return std::byteswap(big(a)); };
  uint32_t at = from;
  while (at < to) {
    const uint32_t h = big(at);
    at += 4;
    const uint32_t count = ((h >> 16) & 0x3FFF) + 1, predicated = h & 1;
    if ((h >> 30) == 0) {
      for (uint32_t i = 0; i < count; ++i) out.push_back({0, (h & 0x7FFF) + i, big(at + 4 * i), 0});
    } else {
      const uint32_t opcode = (h >> 8) & 0x7F;
      CHECK((h >> 30) == 3);
      if (opcode == 0x2D) {
        const uint32_t offset_type = big(at);
        CHECK(((offset_type >> 16) & 0xFF) == 4);
        for (uint32_t i = 1; i < count; ++i) out.push_back({0, 0x2000 + (offset_type & 0x7FF) + i - 1, big(at + 4 * i), predicated});
      } else if (opcode >= 0x60 && opcode <= 0x63) {
        CHECK(count == 1);
        out.push_back({3, opcode - 0x60, big(at), predicated});
      } else {
        const uint32_t kind = host(at);
        CHECK(opcode == record::kOpcode);
        if ((kind & 0xFF) == record::kBins) {
          CHECK(count == record::kBinsWords);
          out.push_back({3, kind >> 8, host(at + 4), predicated});
        } else {
          CHECK((kind & 0xFF) == record::kRegisters);
          for (uint32_t i = 1; i < count; ++i) out.push_back({0, (kind >> 8) + i - 1, host(at + 4 * i), predicated});
        }
      }
    }
    at += 4 * count;
  }
  CHECK(at == to);
  return out;
}

// The viewport and scissor functions against their code read line by line: the window scissor's packets (PM4 byte for
// byte; records decoded against the PM4), the scissor, the viewport on its floats (doubles that are not floats, NaN,
// infinities, huge, negative), SetViewport on a D3DVIEWPORT9 (any words) and a surface's viewport. The device's
// fields at random: the bound and recorded surfaces, the tiling's flags, options, tile count (0-15, and past it for the
// packets), rects, offsets, bin masks and size, the scissor's enable, the viewport as set, the surfaces' words 24 and
// 36; the command buffer's room (short of the packets now and then). The runtime's 0.5, viewport and rect in the arena.
// The viewport functions' packets: the model's (PM4) on one side, the reference's on the other.
void TestViewportChain(xdk::D3dLayout l, uint32_t seed) {
  std::mt19937_64 rng(seed);
  l.blend_rounding = 0x50018, l.surface_viewport = 0x50040, l.surface_scissor = 0x50060;
  auto surface_at = [](uint32_t k) { return 0x60000u + 64 * k; };
  auto coordinate = [&]() -> uint32_t {
    switch (rng() % 6) {
      case 0: return uint32_t(rng());
      case 1: return uint32_t(int32_t(rng() % 512) - 256);
      default: return uint32_t(rng() % 2048);
    }
  };
  auto real = [&]() -> double {
    switch (rng() % 10) {
      case 0: return std::numeric_limits<double>::quiet_NaN();
      case 1: return rng() % 2 ? HUGE_VAL : -HUGE_VAL;
      case 2: return double(int64_t(rng() % 0x200000000ull) - 0x100000000ll) + 0.5;  // past the 32-bit range
      case 3: return std::bit_cast<double>(rng());
      case 4: return double(rng() % 4096) + double(rng() % 1000) / 997.0;  // not a float
      case 5: return -double(rng() % 300) - 0.25;
      default: return double(float(rng() % 2048));
    }
  };
  uint64_t taken = 0, records_compared = 0, declined = 0;
  for (int round = 0; round < 200; ++round) {
    Pair p(seed + round);
    Put32Both(p, l.blend_rounding, rng() % 4 ? std::bit_cast<uint32_t>(0.5f) : std::bit_cast<uint32_t>(RandomFloat(rng)));
    for (uint32_t k = 0; k < 6; ++k) Put32Both(p, l.surface_viewport + 4 * k, k < 4 ? coordinate() : uint32_t(rng()));
    for (uint32_t k = 0; k < 4; ++k) Put32Both(p, l.surface_scissor + 4 * k, coordinate());
    for (int call = 0; call < 40; ++call) {
      for (uint32_t k = 0; k < 5; ++k) {
        const uint32_t bound = rng() % 3 ? surface_at(uint32_t(rng() % 8)) : 0;
        if (call == 0 || rng() % 3 == 0) Put32Both(p, kDevice + l.surfaces + 4 * k, bound);  // the arena's surfaces
        if (rng() % 3 == 0) Put32Both(p, kDevice + l.tiled_surfaces + 4 * k, rng() % 2 ? bound : surface_at(uint32_t(rng() % 8)));
      }
      for (uint32_t at : {l.tiling_flags, l.tiling_options}) {
        if (rng() % 3 == 0) *p.a.Host(kDevice + at) = *p.b.Host(kDevice + at) = uint8_t(rng());
      }
      const uint32_t which = uint32_t(rng() % 6);
      const bool chain = which >= 2;
      Put32Both(p, kDevice + l.tile_count, chain || rng() % 8 ? uint32_t(rng() % 16) : 16 + uint32_t(rng() % 4));
      for (uint32_t k = 0; k < 15; ++k) {
        for (uint32_t w = 0; w < 4; ++w) Put32Both(p, kDevice + l.tile_rects + 16 * k + 4 * w, coordinate());
        for (uint32_t w = 0; w < 2; ++w) Put32Both(p, kDevice + l.tile_offsets + 8 * k + 4 * w, coordinate());
      }
      for (uint32_t w = 0; w < 2; ++w) {
        Put32Both(p, kDevice + l.tile_bins + 4 * w, uint32_t(rng()));
        Put32Both(p, kDevice + l.tiled_size + 4 * w, rng() % 4 ? uint32_t(rng() % 4096) : uint32_t(rng()));
        Put32Both(p, kDevice + l.window_scissor + 4 * w, uint32_t(rng()));
      }
      Put32Both(p, kDevice + l.scissor_enable, rng() % 3 ? uint32_t(rng() % 2) : uint32_t(rng()));
      for (uint32_t k = 0; k < 4; ++k) Put32Both(p, kDevice + l.viewport_set + 4 * k, std::bit_cast<uint32_t>(float(real())));
      for (uint32_t k = 0; k < 8; ++k) {
        Put32Both(p, surface_at(k) + 24, uint32_t(rng()));
        Put32Both(p, surface_at(k) + 36, uint32_t(rng()));
      }
      const uint32_t cursor = 0x40000 + 4 * uint32_t(rng() % 256);
      const uint32_t room = chain || rng() % 2 ? 0x2000 : 4 * uint32_t(rng() % 160);
      Put32Both(p, kDevice + l.command_cursor, cursor);
      Put32Both(p, kDevice + l.command_end, chain || rng() % 16 ? cursor + room : cursor - 4 * uint32_t(1 + rng() % 8));
      // The model's packets (PM4) on one side, the reference's on the other; both see the same arguments.
      std::vector<std::array<uint32_t, 4>> args_a, args_b;
      auto packets_a = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        args_a.push_back({a, b, c, d});
        CHECK(xdk::D3dWindowScissorPackets(p.a, l, kDevice, a, b, c, d, false));
      };
      auto packets_b = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        args_b.push_back({a, b, c, d});
        ref::WindowScissorPackets(p.b, l, kDevice, a, b, c, d);
      };
      bool took = false;
      switch (which) {
        case 0:
        case 1: {
          const uint32_t left = coordinate(), top = coordinate(), right = coordinate(), bottom = coordinate();
          // The PM4 form's size, as the function writes it: what it takes when there is room for it.
          const bool tiled = ref::TiledTest(p.b, l, kDevice);
          const uint32_t tiles = ref::lwz(p.b, kDevice + l.tile_count);
          const bool whole = (ref::lbz(p.b, kDevice + l.tiling_options) & 0x40) && (ref::lbz(p.b, kDevice + l.tiling_flags) & 0x40);
          const bool records = which == 1;
          const uint32_t bins = records ? 3 : 2;
          const uint64_t words = !tiled ? (records ? 5 : 4) : bins + uint64_t(tiles) * (bins + 5) + (whole ? bins + 4 : 0) + 2 * bins;
          const bool expect = (!tiled || tiles <= 15) && cursor + 4 * words <= ref::lwz(p.b, kDevice + l.command_end);
          took = xdk::D3dWindowScissorPackets(p.a, l, kDevice, left, top, right, bottom, records);
          CHECK(took == expect);
          if (!took) {
            ++declined;
            break;
          }
          ref::WindowScissorPackets(p.b, l, kDevice, left, top, right, bottom);
          if (records) {
            const uint32_t end_a = ref::lwz(p.a, kDevice + l.command_cursor) + 4, end_b = ref::lwz(p.b, kDevice + l.command_cursor) + 4;
            CHECK(end_a == cursor + 4 + 4 * words);
            CHECK(ScissorCommands(p.native, cursor + 4, end_a) == ScissorCommands(p.model, cursor + 4, end_b));
            ++records_compared;
            // The PM4 side's bytes on both, to go on comparing everything else.
            std::memcpy(&p.native[cursor], &p.model[cursor], std::max(end_a, end_b) - cursor);
            std::memcpy(&p.native[kDevice + l.command_cursor], &p.model[kDevice + l.command_cursor], 4);
          }
          break;
        }
        case 2: {
          const uint32_t rect = rng() % 4 ? 0x61000 + 4 * uint32_t(rng() % 64) : kDevice + l.scissor_set;
          if (rect != kDevice + l.scissor_set) {
            for (uint32_t w = 0; w < 4; ++w) Put32Both(p, rect + 4 * w, coordinate());
          }
          took = xdk::D3dApplyScissor(p.a, l, kDevice, rect, packets_a);
          ref::ApplyScissor(p.b, l, kDevice, rect, packets_b);
          break;
        }
        case 3: {
          const double x = real(), y = real(), w = real(), h = real(), min_z = real(), max_z = real();
          took = xdk::D3dSetViewportFloats(p.a, l, kDevice, x, y, w, h, min_z, max_z, packets_a);
          ref::SetViewportFloats(p.b, l, kDevice, x, y, w, h, min_z, max_z, packets_b);
          break;
        }
        case 4: {
          const uint32_t viewport = 0x62000 + 4 * uint32_t(rng() % 32);
          for (uint32_t k = 0; k < 6; ++k) {
            Put32Both(p, viewport + 4 * k, k < 4 ? (rng() % 4 ? coordinate() : uint32_t(rng())) : std::bit_cast<uint32_t>(float(real())));
          }
          took = xdk::D3dSetViewport(p.a, l, kDevice, viewport, packets_a);
          ref::SetViewport(p.b, l, kDevice, viewport, packets_b);
          break;
        }
        default: {
          const uint32_t surface = surface_at(uint32_t(rng() % 8));
          took = xdk::D3dViewportOfSurface(p.a, l, kDevice, surface, packets_a);
          ref::ViewportOfSurface(p.b, l, kDevice, surface, packets_b);
          break;
        }
      }
      if (chain) CHECK(took);
      CHECK(args_a == args_b);
      taken += took;
      CHECK(p.Same(kDevice, kDevice + l.device_bytes));
      CHECK(p.Same(cursor, cursor + 0x2000));
    }
    CHECK(p.SameEverywhere());
    if (!p.SameEverywhere()) std::memcpy(p.model.data(), p.native.data(), kArena);
  }
  CHECK(taken > 5000 && records_compared > 500 && declined > 300);
  // Declined, memory untouched: a rect, a viewport or a surface not plain, the device in the GPU's register window,
  // the runtime's viewport over the device, a bound surface's word 36 not plain (untiled).
  Pair p(seed), q(seed);
  auto none = [](uint32_t, uint32_t, uint32_t, uint32_t) {};
  CHECK(!xdk::D3dApplyScissor(p.a, l, kDevice, 0x7F000000u - 8, none));
  CHECK(!xdk::D3dSetViewport(p.a, l, kDevice, 0xFFFFFFF0u, none));
  CHECK(!xdk::D3dViewportOfSurface(p.a, l, kDevice, 0x7F000000u - 24, none));
  CHECK(!xdk::D3dSetViewportFloats(p.a, l, 0x7F000000u - 64, 0, 0, 1, 1, 0, 1, none));
  CHECK(!xdk::D3dWindowScissorPackets(p.a, l, 0x7F000000u - 64, 0, 0, 1, 1, false));
  xdk::D3dLayout over = l;
  over.surface_viewport = kDevice + 64;
  CHECK(!xdk::D3dViewportOfSurface(p.a, over, kDevice, surface_at(0), none));
  CHECK(p.native == q.native);
  Put32Both(p, kDevice + l.surfaces, 0x7F000000u - 38), Put32Both(q, kDevice + l.surfaces, 0x7F000000u - 38);
  *p.a.Host(kDevice + l.tiling_flags) = *q.a.Host(kDevice + l.tiling_flags) = 0;
  CHECK(!xdk::D3dSetViewportFloats(p.a, l, kDevice, 0, 0, 1, 1, 0, 1, none));
  CHECK(p.native == q.native);
}

int main() {
  const xdk::D3dLayout standard, shifted = Shifted();
  TestViewportChain(standard, 2500);
  TestViewportChain(shifted, 2600);
  TestRenderStateFields(standard, 2300);
  TestRenderStateFields(shifted, 2400);
  TestSurfaceSetters(standard, 2100);
  TestSurfaceSetters(shifted, 2200);
  TestRenderStates(standard, 1900);
  TestRenderStates(shifted, 2000);
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
  TestSamplerFilter(standard, 1300);
  TestSamplerFilter(shifted, 1400);
  TestSetVertexDeclaration(standard, 1500);
  TestSetVertexDeclaration(shifted, 1600);
  TestSetShader(standard, 1700);
  TestSetShader(shifted, 1800);
  TestAllocMeter();
  TestTileOps();
  return xdk_test::Finish("xdk_d3d_state_tests");
}
