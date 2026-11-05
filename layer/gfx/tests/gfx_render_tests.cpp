// End to end on a real driver: PM4 written by hand (no game data) runs through the command processor, whose draws
// the Vulkan renderer turns into host draws with translated shaders, vertices fetched from guest memory, constants
// from the register file and indices by DMA; the render state comes from the registers. Each scenario clears the
// targets, runs its packets and checks the pixels it read back. Skipped without a Vulkan 1.3 device.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>
#include <functional>
#include <string>
#include <vector>

#include "any360/gfx/capture.h"
#include "any360/gfx/command_processor.h"
#include "any360/gfx/vulkan/renderer.h"
#include "ucode_encode.h"

namespace {

using namespace any360::gfx;
using pm4::Type0;
using pm4::Type3;
namespace t = any360::gfx::test;
namespace uc = rex::graphics::ucode;

int g_failures = 0;
#define CHECK(condition)                                                  \
  do {                                                                    \
    if (!(condition)) {                                                   \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition);         \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

constexpr uint32_t kMemory = 0x200000, kRing = 0x10000, kVertices = 0x40000, kIndices = 0x48000, kSize = 64;
// Guest memory: page-aligned so the renderer can import it in place (--copy shifts it off alignment, so the
// renderer takes its copying path instead).
uint8_t* const g_storage = [] {
  auto* p = static_cast<uint8_t*>(::operator new(kMemory + 64, std::align_val_t(65536)));
  std::memset(p, 0, kMemory + 64);
  return p;
}();
uint8_t* g_memory = g_storage;
// --capture PATH: the capture scenario's file is kept there (for gfx_replay) instead of deleted.
std::string g_capture;

// Register indices the scenarios write.
constexpr uint32_t kRbDepthControl = 0x2200, kRbBlendControl0 = 0x2201, kRbColorMask = 0x2104,
                   kRbStencilRefMask = 0x210D, kPaSuScModeCntl = 0x2205, kPaScWindowScissorTl = 0x2081,
                   kPaScWindowScissorBr = 0x2082;
// Xenos primitive types.
constexpr uint32_t kTriangleList = 4, kTriangleStrip = 6;

void SetBig(uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(g_memory + address, &value, 4);
}
uint32_t Bits(float f) { return std::bit_cast<uint32_t>(f); }
uint32_t Rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return a << 24 | b << 16 | g << 8 | r; }

// Vertex sets (float4 each, big-endian), set k at kVertices + 0x100 * k: a quad in strip order (lower-left,
// lower-right, upper-left, upper-right) at depth z; the lower-left triangle is its first three vertices.
void WriteQuad(uint32_t set, float z) {
  const float v[4][4] = {{-1, -1, z, 1}, {1, -1, z, 1}, {-1, 1, z, 1}, {1, 1, z, 1}};
  for (uint32_t i = 0; i < 4; ++i) {
    for (uint32_t c = 0; c < 4; ++c) SetBig(kVertices + 0x100 * set + 16 * i + 4 * c, Bits(v[i][c]));
  }
}

struct Scene {
  CommandProcessor& cp;
  uint32_t write = 0;

  void Put(uint32_t dword) {
    SetBig(kRing + 4 * write, dword);
    write = (write + 1) % 8192;
  }
  void Put(std::initializer_list<uint32_t> dwords) {
    for (uint32_t d : dwords) Put(d);
  }
  void Reg(uint32_t index, uint32_t value) { Put({Type0(index, 1), value}); }
  void Shader(uint32_t type, const std::vector<uint32_t>& words) {
    Put({Type3(pm4::IM_LOAD_IMMEDIATE, 2 + uint32_t(words.size())), type, uint32_t(words.size())});
    for (uint32_t w : words) Put(w);
  }
  // Pixel shader c0 is float constant 256: dword 1024 of the ALU constants.
  void Color(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    Put({Type3(pm4::SET_CONSTANT, 5), 0u << 16 | 1024, Bits(r / 255.0f), Bits(g / 255.0f), Bits(b / 255.0f), Bits(a / 255.0f)});
  }
  void Vertices(uint32_t set) { Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, (kVertices + 0x100 * set) | 3, 2u | 16u << 2}); }
  void DrawAuto(uint32_t primitive, uint32_t count) { Put({Type3(pm4::DRAW_INDX_2, 1), count << 16 | 2u << 6 | primitive}); }
  void DrawIndexed16(uint32_t primitive, uint32_t count) {
    Put({Type3(pm4::DRAW_INDX, 4), 0, count << 16 | 0u << 6 | primitive, kIndices, 1u << 30 | count});
  }
  // The state every scenario starts from: colour+depth, a D3D viewport over the target (y up, W as is), the full
  // target as screen and window scissor, no depth, stencil or blending, all colour written, no culling.
  void Defaults() {
    // Surfaces: pitch 64, colour 0 at EDRAM tile 0 (k_8_8_8_8), depth at tile 1024 (D24S8).
    Put({Type0(0x2000, 3), kSize, 0u << 16 | 0, 0u << 16 | 1024});
    Reg(0x2208, 4);
    // Indices: the full range (VGT_MAX_VTX_INDX, VGT_MIN_VTX_INDX), no offset, no primitive reset.
    Put({Type0(0x2100, 4), 0xFFFFFF, 0, 0, 0});
    Reg(0x2206, 0x3F | 1u << 10);
    Put({Type0(0x210F, 6), Bits(kSize / 2.0f), Bits(kSize / 2.0f), Bits(-float(kSize) / 2.0f), Bits(kSize / 2.0f), Bits(1), Bits(0)});
    Put({Type0(0x200E, 2), 0, kSize << 16 | kSize});
    Put({Type0(0x2080, 3), 0, 0, kSize << 16 | kSize});
    Reg(kRbDepthControl, 0);
    Reg(kRbBlendControl0, 0x00010001);
    Put({Type0(0x2209, 3), 0x00010001, 0x00010001, 0x00010001});  // RB_BLENDCONTROL1-3: 0 would blend ZERO + ZERO
    Reg(kRbColorMask, 0xF);
    Reg(kPaSuScModeCntl, 0);
  }
  void Run() {
    cp.WriteRegister(reg::CP_RB_WPTR, write);
    CHECK(cp.Execute());
  }
};

struct Pixels {
  std::vector<uint32_t> data;
  uint32_t At(uint32_t x, uint32_t y) const { return data[y * kSize + x]; }
  uint32_t Count(uint32_t color) const {
    uint32_t n = 0;
    for (uint32_t p : data) n += p == color;
    return n;
  }
};

const uint32_t kBlack = Rgba(0, 0, 0, 255);
// The pixels the lower-left triangle covers (centres strictly below the anti-diagonal; the diagonal itself goes to
// one side or the other by the rasterization rules).
bool LowerLeft(uint32_t x, uint32_t y) { return x + (kSize - 1 - y) < kSize - 1; }
bool OnDiagonal(uint32_t x, uint32_t y) { return x + (kSize - 1 - y) == kSize - 1; }

}  // namespace

int main(int argc, char** argv) {
  vk::Device device;
  vk::DeviceOptions options;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--copy")) g_memory = g_storage + 64;
    else if (!std::strcmp(argv[i], "--capture") && i + 1 < argc) g_capture = argv[++i];
    else options.prefer = argv[i];
  }
  if (!device.Create(options)) {
    std::printf("gfx_render_tests: skipped (%s)\n", device.error().c_str());
    return 0;
  }
  vk::VulkanRenderer renderer(device, g_memory, kMemory);
  CHECK(renderer.Initialize());
  const vk::SurfaceKey color0{0, kSize, 0, 0}, depth{1024, kSize, 0, 1};
  auto read = [&](const vk::SurfaceKey& key) {
    Pixels p;
    std::vector<uint8_t> bytes;
    CHECK(renderer.ReadBack(key, kSize, kSize, bytes));
    p.data.assign(kSize * kSize, 0);
    if (bytes.size() == kSize * kSize * 4) std::memcpy(p.data.data(), bytes.data(), bytes.size());
    return p;
  };
  CommandProcessor cp(g_memory);
  cp.SetRenderer(&renderer);
  cp.InitializeRingBuffer({}, kRing, 12);  // 32 KB: 8192 dwords
  Scene s{cp};

  WriteQuad(0, 0.5f);
  WriteQuad(1, 0.75f);
  WriteQuad(2, 0.25f);
  const uint16_t indices[3] = {1, 3, 2};  // the upper-right triangle of a quad
  for (uint32_t i = 0; i < 3; ++i) {
    const uint16_t big = uint16_t(indices[i] << 8 | indices[i] >> 8);
    std::memcpy(g_memory + kIndices + 2 * i, &big, 2);
  }
  s.Shader(0, t::BuildFetchPositionShader());
  s.Shader(1, t::BuildSingleExportShader(uc::AllocType::kPsColors, uint32_t(uc::ExportRegister::kPSColor0), {0, false}));

  const float black[4] = {0, 0, 0, 1};
  auto scenario = [&](const char* name, const float* clear, const std::function<void()>& packets) {
    const uint32_t failed_before = renderer.stats().failed;
    CHECK(renderer.ClearColor(color0, clear ? clear : black));
    CHECK(renderer.ClearDepthStencil(depth, 1.0f, 0));
    s.Defaults();
    packets();
    s.Run();
    if (renderer.stats().failed != failed_before) std::printf("gfx_render_tests: %s: %s\n", name, renderer.last_error().c_str());
    Pixels p = read(color0);
    return p;
  };

  // Two triangles: the lower-left auto-indexed, the upper-right by DMA with 16-bit indices swapped 8-in-16.
  {
    const uint32_t a = Rgba(51, 102, 153, 255), b = Rgba(204, 51, 102, 255);
    Pixels p = scenario("triangles", nullptr, [&] {
      s.Vertices(0);
      s.Color(51, 102, 153, 255);
      s.DrawAuto(kTriangleList, 3);
      s.Color(204, 51, 102, 255);
      s.DrawIndexed16(kTriangleList, 3);
    });
    CHECK(p.At(4, 59) == a && p.At(59, 4) == b && p.Count(a) + p.Count(b) == kSize * kSize);
    std::printf("gfx_render_tests: triangles %u + %u pixels\n", p.Count(a), p.Count(b));
  }
  // Primitive reset and index offset: a triangle strip of DMA indices {0, 1, 2, reset, 1, 3, 2} with reset index
  // 0x1234 (PA_SU_SC_MODE_CNTL multi_prim_ib_ena) is the two triangles of a quad; the vertex fetch constant points
  // at vertex set 5 (all zero: nothing would be drawn) and VGT_INDX_OFFSET 16 moves the indices to set 6's quad
  // (indices are 24-bit: the offset cannot reach below the fetch constant's base).
  {
    const uint32_t strip_indices = kIndices + 0x100;
    WriteQuad(6, 0.5f);
    const uint16_t strip[7] = {0, 1, 2, 0x1234, 1, 3, 2};
    for (uint32_t i = 0; i < 7; ++i) {
      const uint16_t big = uint16_t(strip[i] << 8 | strip[i] >> 8);
      std::memcpy(g_memory + strip_indices + 2 * i, &big, 2);
    }
    const uint32_t color = Rgba(10, 200, 30, 255);
    Pixels p = scenario("primitive reset", nullptr, [&] {
      s.Reg(kPaSuScModeCntl, 1u << 21);
      s.Reg(0x2103, 0x1234);           // VGT_MULTI_PRIM_IB_RESET_INDX
      s.Reg(0x2102, 16);               // VGT_INDX_OFFSET
      s.Vertices(5);
      s.Color(10, 200, 30, 255);
      s.Put({Type3(pm4::DRAW_INDX, 4), 0, 7u << 16 | 0u << 6 | kTriangleStrip, strip_indices, 1u << 30 | 7u});
      s.Put({Type0(0x2102, 2), 0, 0});
    });
    std::printf("gfx_render_tests: primitive reset and index offset: %u of %u pixels\n", p.Count(color), kSize * kSize);
    CHECK(p.Count(color) == kSize * kSize);
  }
  // A rectangle list (three corners; the vertex shader makes the fourth) and a quad list (corners in order around
  // the quad, drawn as two triangles), each auto-indexed over the full target.
  {
    const float perimeter[4][4] = {{-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}, {1, 1, 0.5f, 1}, {-1, 1, 0.5f, 1}};
    for (uint32_t i = 0; i < 4; ++i) {
      for (uint32_t j = 0; j < 4; ++j) SetBig(kVertices + 0x700 + 16 * i + 4 * j, Bits(perimeter[i][j]));
    }
    const uint32_t color = Rgba(90, 20, 220, 255);
    Pixels p = scenario("rectangle list", nullptr, [&] {
      s.Vertices(0);
      s.Color(90, 20, 220, 255);
      s.DrawAuto(8, 3);
    });
    std::printf("gfx_render_tests: rectangle list: %u of %u pixels\n", p.Count(color), kSize * kSize);
    CHECK(p.Count(color) == kSize * kSize);
    p = scenario("quad list", nullptr, [&] {
      s.Vertices(7);
      s.Color(90, 20, 220, 255);
      s.DrawAuto(13, 4);
    });
    std::printf("gfx_render_tests: quad list: %u of %u pixels\n", p.Count(color), kSize * kSize);
    CHECK(p.Count(color) == kSize * kSize);
  }
  // MSAA as samples: a full-target quad drawn with RB_SURFACE_INFO at 4x MSAA and pitch 32 (a 32x32 viewport and
  // scissor) covers the 64x64 samples of the same EDRAM, the 1x surface of pitch 64.
  {
    const uint32_t color = Rgba(200, 120, 10, 255);
    Pixels p = scenario("MSAA as samples", nullptr, [&] {
      s.Reg(0x2000, 32u | 2u << 16);  // RB_SURFACE_INFO: pitch 32, 4x MSAA
      s.Reg(0x2302, 1);               // PA_SU_VTX_CNTL: OpenGL pixel centres (D3D's would move the quad half a pixel)
      s.Put({Type0(0x210F, 6), Bits(16), Bits(16), Bits(-16), Bits(16), Bits(1), Bits(0)});
      s.Put({Type0(0x200E, 2), 0, 32u << 16 | 32u});
      s.Put({Type0(0x2080, 3), 0, 0, 32u << 16 | 32u});
      s.Vertices(0);
      s.Color(200, 120, 10, 255);
      s.DrawAuto(kTriangleStrip, 4);
      s.Reg(0x2302, 0);
    });
    std::printf("gfx_render_tests: MSAA as samples: %u of %u samples\n", p.Count(color), kSize * kSize);
    CHECK(p.Count(color) == kSize * kSize);
  }
  // Depth: a quad at 0.5 (LESS, writes), one behind at 0.75 hidden, a triangle in front at 0.25 shown.
  {
    const uint32_t mid = Rgba(0, 255, 0, 255), back = Rgba(255, 0, 0, 255), front = Rgba(0, 0, 255, 255);
    Pixels p = scenario("depth", nullptr, [&] {
      s.Reg(kRbDepthControl, 1u << 1 | 1u << 2 | 1u << 4);  // z_enable, z_write, LESS
      s.Vertices(0);
      s.Color(0, 255, 0, 255);
      s.DrawAuto(kTriangleStrip, 4);
      s.Vertices(1);
      s.Color(255, 0, 0, 255);
      s.DrawAuto(kTriangleStrip, 4);
      s.Vertices(2);
      s.Color(0, 0, 255, 255);
      s.DrawAuto(kTriangleList, 3);
    });
    std::printf("gfx_render_tests: depth front %u, middle %u, back %u\n", p.Count(front), p.Count(mid), p.Count(back));
    CHECK(p.Count(back) == 0 && p.At(4, 59) == front && p.At(59, 4) == mid);
    CHECK(p.Count(front) + p.Count(mid) == kSize * kSize && p.Count(front) > 1900);
    // The same with a D24FS8 depth surface (RB_DEPTH_INFO depth_format 1: float24 depth).
    CHECK(renderer.ClearDepthStencil({1024, kSize, 1, 1}, 1.0f, 0));
    p = scenario("float24 depth", nullptr, [&] {
      s.Reg(0x2002, 1u << 16 | 1024);
      s.Reg(kRbDepthControl, 1u << 1 | 1u << 2 | 1u << 4);
      s.Vertices(0);
      s.Color(0, 255, 0, 255);
      s.DrawAuto(kTriangleStrip, 4);
      s.Vertices(1);
      s.Color(255, 0, 0, 255);
      s.DrawAuto(kTriangleStrip, 4);
      s.Vertices(2);
      s.Color(0, 0, 255, 255);
      s.DrawAuto(kTriangleList, 3);
    });
    std::printf("gfx_render_tests: float24 depth front %u, middle %u, back %u\n", p.Count(front), p.Count(mid), p.Count(back));
    CHECK(p.Count(back) == 0 && p.At(4, 59) == front && p.At(59, 4) == mid);
    CHECK(p.Count(front) + p.Count(mid) == kSize * kSize && p.Count(front) > 1900);
  }
  // Primitive types the host draws otherwise: a triangle fan and a polygon over the four corners in order around
  // the target, a quad strip over them in strip order, each covering it all; a line list, strip and loop over the
  // corners of a square inset to +-0.7, each one more edge than the one before (about 45 pixels an edge).
  {
    const float inset[4][4] = {{-0.7f, -0.7f, 0.5f, 1}, {0.7f, -0.7f, 0.5f, 1}, {0.7f, 0.7f, 0.5f, 1}, {-0.7f, 0.7f, 0.5f, 1}};
    for (uint32_t i = 0; i < 4; ++i) {
      for (uint32_t c = 0; c < 4; ++c) SetBig(kVertices + 0x900 + 16 * i + 4 * c, Bits(inset[i][c]));
    }
    const uint32_t color = Rgba(30, 160, 90, 255);
    const struct {
      const char* name;
      uint32_t primitive, set;
    } fills[] = {{"triangle fan", 5, 7}, {"polygon", 15, 7}, {"quad strip", 14, 0}};
    for (const auto& fill : fills) {
      Pixels p = scenario(fill.name, nullptr, [&] {
        s.Vertices(fill.set);
        s.Color(30, 160, 90, 255);
        s.DrawAuto(fill.primitive, 4);
      });
      std::printf("gfx_render_tests: %s: %u of %u pixels\n", fill.name, p.Count(color), kSize * kSize);
      CHECK(p.Count(color) == kSize * kSize);
    }
    uint32_t lit[3] = {};
    const uint32_t lines[3] = {2, 3, 12};  // line list, line strip, line loop
    for (uint32_t i = 0; i < 3; ++i) {
      Pixels p = scenario("lines", nullptr, [&] {
        s.Vertices(9);
        s.Color(30, 160, 90, 255);
        s.DrawAuto(lines[i], 4);
      });
      lit[i] = p.Count(color);
    }
    std::printf("gfx_render_tests: line list %u, strip %u, loop %u pixels\n", lit[0], lit[1], lit[2]);
    CHECK(lit[0] >= 80 && lit[0] <= 100 && lit[1] >= lit[0] + 40 && lit[1] <= lit[0] + 50 && lit[2] >= lit[1] + 40 &&
          lit[2] <= lit[1] + 50);
  }
  // Render target formats: c0 written over the whole target of each format, read back as the host stores it.
  {
    struct Target {
      const char* name;
      uint32_t format;
      float c[4];
      uint32_t bytes;     // read back a texel
      int64_t expect[4];  // host components (UNORM8 bytes, SNORM16, half or float bits)
    };
    const Target targets[] = {
        {"k_8_8_8_8_GAMMA", 1, {1, 0, 1, 0}, 4, {255, 0, 255, 0}},
        {"k_2_10_10_10", 2, {0.2f, 0.6f, 1, 0.4f}, 4, {51, 153, 255, 102}},
        {"k_2_10_10_10_AS_10_10_10_10", 10, {0.2f, 0.6f, 1, 0.4f}, 4, {51, 153, 255, 102}},
        {"k_2_10_10_10_FLOAT", 3, {0.5f, -2, 1024, 0.125f}, 8, {0x3800, 0xC000, 0x6400, 0x3000}},
        {"k_2_10_10_10_FLOAT_AS_16_16_16_16", 12, {0.5f, -2, 1024, 0.125f}, 8, {0x3800, 0xC000, 0x6400, 0x3000}},
        // The 16-bit fixed-point formats hold -32...32: the shader's 8 is a quarter of full scale.
        {"k_16_16", 4, {8, -16, 0, 0}, 4, {8192, -16384}},
        {"k_16_16_16_16", 5, {8, -16, 16, -8}, 8, {8192, -16384, 16384, -8192}},
        {"k_16_16_FLOAT", 6, {0.5f, -2, 0, 0}, 4, {0x3800, 0xC000}},
        {"k_32_FLOAT", 14, {3.25f, 0, 0, 0}, 4, {int64_t(Bits(3.25f))}},
    };
    for (const Target& target : targets) {
      const vk::SurfaceKey key{1536, kSize, target.format, 0};
      const float zero[4] = {};
      CHECK(renderer.ClearColor(key, zero));
      scenario(target.name, nullptr, [&] {
        s.Reg(0x2001, target.format << 16 | 1536);
        s.Vertices(0);
        s.Put({Type3(pm4::SET_CONSTANT, 5), 0u << 16 | 1024, Bits(target.c[0]), Bits(target.c[1]), Bits(target.c[2]),
               Bits(target.c[3])});
        s.DrawAuto(kTriangleStrip, 4);
      });
      std::vector<uint8_t> bytes;
      CHECK(renderer.ReadBack(key, kSize, kSize, bytes));
      int64_t got[4] = {};
      if (bytes.size() == size_t(kSize) * kSize * target.bytes) {
        const uint8_t* texel = bytes.data() + (32 * kSize + 32) * target.bytes;
        const bool unorm8 = target.format == 1 || target.format == 2 || target.format == 10;
        const bool snorm = target.format == 4 || target.format == 5;
        const uint32_t count = target.format == 14 ? 1 : target.bytes == 8 || unorm8 ? 4 : 2;
        for (uint32_t i = 0; i < count; ++i) {
          if (unorm8) {
            got[i] = texel[i];
          } else if (target.format == 14) {
            uint32_t v;
            std::memcpy(&v, texel, 4);
            got[i] = v;
          } else {
            uint16_t v;
            std::memcpy(&v, texel + 2 * i, 2);
            got[i] = snorm ? int16_t(v) : v;
          }
        }
      }
      bool ok = true;
      for (uint32_t i = 0; i < 4; ++i) ok &= std::llabs(got[i] - target.expect[i]) <= 1;
      std::printf("gfx_render_tests: target %s: %lld %lld %lld %lld%s\n", target.name, (long long)got[0], (long long)got[1],
                  (long long)got[2], (long long)got[3], ok ? "" : " (wrong)");
      CHECK(ok);
    }
  }
  // Blending: ONE + ONE over a grey clear: (51, 51, 51) + (51, 102, 0) = (102, 153, 51).
  {
    const float grey[4] = {51 / 255.0f, 51 / 255.0f, 51 / 255.0f, 1};
    Pixels p = scenario("blend", grey, [&] {
      s.Reg(kRbBlendControl0, 0x01010101);  // colour and alpha: src ONE, ADD, dst ONE
      s.Vertices(0);
      s.Color(51, 102, 0, 0);
      s.DrawAuto(kTriangleStrip, 4);
    });
    CHECK(p.Count(Rgba(102, 153, 51, 255)) == kSize * kSize);
  }
  // Colour write mask: red only.
  {
    Pixels p = scenario("mask", nullptr, [&] {
      s.Reg(kRbColorMask, 0x1);
      s.Vertices(0);
      s.Color(255, 255, 255, 0);
      s.DrawAuto(kTriangleStrip, 4);
    });
    CHECK(p.Count(Rgba(255, 0, 0, 255)) == kSize * kSize);
  }
  // Culling, D3D semantics: the lower-left triangle winds counter-clockwise on screen; with clockwise front faces
  // (face = 1) it is a back face, culled by cull_back and kept by cull_front.
  {
    const uint32_t white = Rgba(255, 255, 255, 255);
    auto drawn = [&](uint32_t mode_cntl) {
      Pixels p = scenario("cull", nullptr, [&] {
        s.Reg(kPaSuScModeCntl, mode_cntl);
        s.Vertices(0);
        s.Color(255, 255, 255, 255);
        s.DrawAuto(kTriangleList, 3);
      });
      return p.Count(white);
    };
    const uint32_t cw_cull_back = drawn(1u << 2 | 1u << 1), cw_cull_front = drawn(1u << 2 | 1u << 0);
    const uint32_t ccw_cull_back = drawn(1u << 1), both = drawn(3);
    std::printf("gfx_render_tests: cull pixels cw+back %u, cw+front %u, ccw+back %u, both %u\n", cw_cull_back,
                cw_cull_front, ccw_cull_back, both);
    CHECK(cw_cull_back == 0 && cw_cull_front > 1900 && ccw_cull_back > 1900 && both == 0);
  }
  // Scissor: the window scissor (16, 16)-(48, 48).
  {
    Pixels p = scenario("scissor", nullptr, [&] {
      s.Reg(kPaScWindowScissorTl, 16u << 16 | 16u);
      s.Reg(kPaScWindowScissorBr, 48u << 16 | 48u);
      s.Vertices(0);
      s.Color(255, 255, 255, 255);
      s.DrawAuto(kTriangleStrip, 4);
    });
    CHECK(p.Count(Rgba(255, 255, 255, 255)) == 32 * 32 && p.At(16, 16) != kBlack && p.At(15, 16) == kBlack &&
          p.At(47, 47) != kBlack && p.At(48, 47) == kBlack);
  }
  // Stencil: the lower-left triangle writes 1 (ALWAYS, REPLACE, no colour), then a quad draws where stencil EQUALS 1.
  {
    const uint32_t yellow = Rgba(255, 255, 0, 255);
    Pixels p = scenario("stencil", nullptr, [&] {
      s.Reg(kRbColorMask, 0);
      s.Reg(kRbStencilRefMask, 0xFFu << 16 | 0xFFu << 8 | 1);
      s.Reg(kRbDepthControl, 1u | 7u << 8 | 2u << 14);  // stencil on, ALWAYS, zpass REPLACE
      s.Vertices(0);
      s.DrawAuto(kTriangleList, 3);
      s.Reg(kRbColorMask, 0xF);
      s.Reg(kRbDepthControl, 1u | 2u << 8);  // stencil on, EQUAL, keep
      s.Color(255, 255, 0, 255);
      s.DrawAuto(kTriangleStrip, 4);
    });
    uint32_t wrong = 0;
    for (uint32_t y = 0; y < kSize; ++y) {
      for (uint32_t x = 0; x < kSize; ++x) {
        if (!OnDiagonal(x, y)) wrong += (p.At(x, y) == yellow) != LowerLeft(x, y);
      }
    }
    std::printf("gfx_render_tests: stencil %u pixels, %u off the triangle\n", p.Count(yellow), wrong);
    CHECK(p.At(4, 59) == yellow && p.At(59, 4) == kBlack && wrong == 0);
  }

  // Several targets and formats: a pixel shader writing oC0 = c0 and oC1 = c1; colour 1 a k_16_16_16_16_FLOAT
  // surface at EDRAM tile 512 (its own host image, apart from colour 0's).

  {
    const vk::SurfaceKey color1{512, kSize, 7, 0};
    const float zero[4] = {0, 0, 0, 0};
    CHECK(renderer.ClearColor(color1, zero));
    Pixels p = scenario("two targets", nullptr, [&] {
      s.Shader(1, t::BuildTwoTargetPixelShader());
      s.Reg(0x2003, 7u << 16 | 512);  // RB_COLOR1_INFO
      s.Reg(kRbColorMask, 0xFF);
      s.Put({Type3(pm4::SET_CONSTANT, 9), 0u << 16 | 1024, Bits(51 / 255.0f), Bits(102 / 255.0f), Bits(153 / 255.0f), Bits(1),
             Bits(0.5f), Bits(-2.0f), Bits(1024.0f), Bits(0.125f)});
      s.Vertices(0);
      s.DrawAuto(kTriangleStrip, 4);
    });
    CHECK(p.Count(Rgba(51, 102, 153, 255)) == kSize * kSize);
    std::vector<uint8_t> bytes;
    CHECK(renderer.ReadBack(color1, kSize, kSize, bytes));
    // Half floats: 0.5 = 0x3800, -2 = 0xC000, 1024 = 0x6400, 0.125 = 0x3000.
    uint32_t exact = 0;
    for (size_t i = 0; i + 8 <= bytes.size(); i += 8) {
      uint16_t h[4];
      std::memcpy(h, bytes.data() + i, 8);
      exact += h[0] == 0x3800 && h[1] == 0xC000 && h[2] == 0x6400 && h[3] == 0x3000;
    }
    if (bytes.size() >= 8) {
      uint16_t h[4];
      std::memcpy(h, bytes.data(), 8);
      std::printf("gfx_render_tests: colour 1 first pixel %04X %04X %04X %04X\n", h[0], h[1], h[2], h[3]);
    }
    std::printf("gfx_render_tests: two targets: colour 1 (16_16_16_16_FLOAT) %u of %u pixels exact\n", exact, kSize * kSize);
    CHECK(exact == kSize * kSize);
  }
  // Pixel parameter generation (SQ_PROGRAM_CNTL param_gen, SQ_CONTEXT_MISC param_gen_pos = 0): the pixel shader
  // exports r0, which the rasterizer fills with the pixel's position (x with the back-facing sign, y), 0 for a
  // triangle in z and 0 in w; drawn into a 16_16_16_16_FLOAT target so the positions stay exact.
  {
    const vk::SurfaceKey target{1536, kSize, 7, 0};
    const float zero[4] = {0, 0, 0, 0};
    CHECK(renderer.ClearColor(target, zero));
    scenario("parameter generation", nullptr, [&] {
      s.Shader(1, t::BuildSingleExportShader(uc::AllocType::kPsColors, uint32_t(uc::ExportRegister::kPSColor0), {0, true}));
      s.Reg(0x2001, 7u << 16 | 1536);  // RB_COLOR_INFO: k_16_16_16_16_FLOAT at tile 1536
      s.Reg(0x2180, 1u << 18);         // SQ_PROGRAM_CNTL: param_gen, one register each
      s.Reg(0x2181, 0);                // SQ_CONTEXT_MISC: param_gen_pos 0
      s.Vertices(0);
      s.DrawAuto(kTriangleStrip, 4);
      s.Reg(0x2180, 0);
    });
    std::vector<uint8_t> bytes;
    CHECK(renderer.ReadBack(target, kSize, kSize, bytes));
    auto half = [](uint16_t h) {
      const float magnitude = std::ldexp(float((h & 0x3FF) | ((h >> 10) & 0x1F ? 0x400 : 0)), ((h >> 10) & 0x1F ? (h >> 10) & 0x1F : 1) - 25);
      return h & 0x8000 ? -magnitude : magnitude;
    };
    uint32_t exact = 0, back_facing = 0;
    for (uint32_t y = 0; y < kSize && bytes.size() == kSize * kSize * 8; ++y) {
      for (uint32_t x = 0; x < kSize; ++x) {
        uint16_t h[4];
        std::memcpy(h, bytes.data() + (size_t(y) * kSize + x) * 8, 8);
        exact += std::fabs(half(h[0])) == float(x) && half(h[1]) == float(y) && (h[2] & 0x7FFF) == 0 && h[3] == 0;
        back_facing += (h[0] & 0x8000) != 0;
      }
    }
    std::printf("gfx_render_tests: parameter generation: %u of %u pixels with their own position, %u back-facing\n", exact,
                kSize * kSize, back_facing);
    CHECK(exact == kSize * kSize && (back_facing == 0 || back_facing == kSize * kSize - kSize));
  }
  // The colour exponent bias of RB_COLOR_INFO: +1 doubles what the shader writes (0.2 -> 0.4).
  {
    Pixels p = scenario("exponent bias", nullptr, [&] {
      s.Shader(1, t::BuildSingleExportShader(uc::AllocType::kPsColors, uint32_t(uc::ExportRegister::kPSColor0), {0, false}));
      s.Reg(0x2001, 1u << 20 | 0u << 16 | 0);  // bias +1, k_8_8_8_8, tile 0
      s.Vertices(0);
      s.Color(51, 102, 0, 255);
      s.DrawAuto(kTriangleStrip, 4);
    });
    std::printf("gfx_render_tests: exponent bias +1: %08X\n", p.At(10, 10));
    CHECK(p.Count(Rgba(102, 204, 0, 255)) == kSize * kSize);
  }

  // Textures: a full-target quad with texcoords sampling a 32x32 texture (point filtering, one texel per 2x2
  // pixels) whose quadrants have four colours; linear and tiled k_8_8_8_8, tiled DXT1.
  {
    // Vertices: position xyzw, texcoord uv (top of the target is v = 0).
    const uint32_t quad = kVertices + 0x400;
    const float v[4][6] = {{-1, -1, 0, 1, 0, 1}, {1, -1, 0, 1, 1, 1}, {-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}};
    for (uint32_t i = 0; i < 4; ++i) {
      for (uint32_t c = 0; c < 6; ++c) SetBig(quad + 24 * i + 4 * c, Bits(v[i][c]));
    }
    // Quadrant colours (top-left, top-right, bottom-left, bottom-right), exact in RGB565 for DXT1.
    const uint32_t quadrant[4] = {Rgba(255, 0, 0, 255), Rgba(0, 255, 0, 255), Rgba(0, 0, 255, 255), Rgba(255, 255, 255, 255)};
    const uint16_t quadrant565[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
    auto quadrant_of = [](uint32_t x, uint32_t y, uint32_t half) { return (y >= half ? 2 : 0) + (x >= half ? 1 : 0); };
    struct Case {
      const char* name;
      uint32_t base, format, endian, tiled;
    };
    const Case cases[] = {{"8888 linear", 0x50000, 6, 2, 0}, {"8888 tiled", 0x60000, 6, 2, 1}, {"DXT1 tiled", 0x70000, 18, 1, 1}};
    for (const Case& c : cases) {
      // The texels as the title stores them: big-endian words, rows of 256-byte pitch (linear) or 32x32 tiles.
      if (c.format == 6) {
        for (uint32_t y = 0; y < 32; ++y) {
          for (uint32_t x = 0; x < 32; ++x) {
            const uint32_t at = c.tiled ? uint32_t(vk::TiledOffset2D(int32_t(x), int32_t(y), 32, 2)) : y * 256 + x * 4;
            SetBig(c.base + at, quadrant[quadrant_of(x, y, 16)]);  // 8-in-32 swapped: R in the low byte
          }
        }
      } else {
        for (uint32_t by = 0; by < 8; ++by) {
          for (uint32_t bx = 0; bx < 8; ++bx) {
            const uint32_t at = uint32_t(vk::TiledOffset2D(int32_t(bx), int32_t(by), 8, 3));
            const uint16_t color = quadrant565[quadrant_of(bx, by, 4)];
            const uint8_t block[8] = {uint8_t(color >> 8), uint8_t(color), 0, 0, 0, 0, 0, 0};  // 8-in-16: big-endian halves
            std::memcpy(g_memory + c.base + at, block, 8);
          }
        }
      }
      Pixels p = scenario(c.name, nullptr, [&] {
        s.Shader(0, t::BuildTexturedVertexShader());
        s.Shader(1, t::BuildTexturedPixelShader(1));
        s.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, quad | 3, 2u | 24u << 2});  // vf0: 4 vertices of 6 floats
        // tf1 (fetch dwords 6-11): texture, clamp to edge, pitch 32, tiled; format, endian, base; 32x32; swizzle
        // RGBA, point filtering; 2D.
        s.Put({Type3(pm4::SET_CONSTANT, 7), 1u << 16 | 6, 2u | 2u << 10 | 2u << 13 | 1u << 22 | c.tiled << 31,
               c.format | c.endian << 6 | c.base, 31u | 31u << 13, 0x688u << 1, 0, 1u << 9});
        s.DrawAuto(kTriangleStrip, 4);
      });
      uint32_t wrong = 0;
      for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) wrong += p.At(x, y) != quadrant[quadrant_of(x, y, 32)];
      }
      std::printf("gfx_render_tests: texture %s: %u of %u pixels wrong (top-left %08X, bottom-right %08X)\n", c.name, wrong,
                  kSize * kSize, p.At(5, 5), p.At(60, 60));
      CHECK(wrong == 0);
    }
    // Mip levels: a 128x128 tiled k_8_8_8_8 texture, red, whose level 1 (64x64, at the mip address, its own tiled
    // pitch) is green, drawn over the 64x64 target: one texel a pixel at level 1, so all green. Then a packed mip
    // tail: a 32x16 texture's level 1 (blue), read from its packed offset in the mip tile.
    {
      const uint32_t base = 0xA0000, mips = 0xB0000;
      for (uint32_t y = 0; y < 128; ++y) {
        for (uint32_t x = 0; x < 128; ++x) SetBig(base + uint32_t(vk::TiledOffset2D(int32_t(x), int32_t(y), 128, 2)), Rgba(255, 0, 0, 255));
      }
      for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) SetBig(mips + uint32_t(vk::TiledOffset2D(int32_t(x), int32_t(y), 64, 2)), Rgba(0, 255, 0, 255));
      }
      auto mipped = [&](uint32_t address, uint32_t mip_address, uint32_t width, uint32_t height, bool packed, uint32_t min_level) {
        s.Shader(0, t::BuildTexturedVertexShader());
        s.Shader(1, t::BuildTexturedPixelShader(1));
        s.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, quad | 3, 2u | 24u << 2});
        // tf1: point filters, levels 0-1 (mip filter point), 2D, the mip address (and the packed bit).
        s.Put({Type3(pm4::SET_CONSTANT, 7), 1u << 16 | 6, 2u | 2u << 10 | 2u << 13 | (std::max(width, 32u) / 32) << 22 | 1u << 31,
               6u | 2u << 6 | address, (width - 1) | (height - 1) << 13, 0x688u << 1, min_level << 2 | 1u << 6,
               1u << 9 | (packed ? 1u << 11 : 0u) | mip_address});
        s.DrawAuto(kTriangleStrip, 4);
      };
      const uint32_t green = Rgba(0, 255, 0, 255), blue = Rgba(0, 0, 255, 255);
      Pixels p = scenario("mip levels", nullptr, [&] { mipped(base, mips, 128, 128, false, 0); });
      std::printf("gfx_render_tests: mip levels: %u of %u pixels from level 1\n", p.Count(green), kSize * kSize);
      CHECK(p.Count(green) == kSize * kSize);
      // The packed tail: a 32x16 texture's shorter side is 16, so every level is packed, level 1 (16x8) at y = 8 of
      // the mip address's tile (a wide texture's levels go down the tile). Levels 1-1 (mip_min_level 1): the sampler
      // reads level 1 only.
      const uint32_t tail_base = 0xB8000, tail_mips = 0xB9000;
      for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
          SetBig(tail_base + uint32_t(vk::TiledOffset2D(int32_t(x), int32_t(y), 32, 2)), Rgba(255, 0, 0, 255));
          SetBig(tail_mips + uint32_t(vk::TiledOffset2D(int32_t(x), int32_t(y), 32, 2)), y >= 8 && y < 16 ? blue : Rgba(255, 0, 0, 255));
        }
      }
      p = scenario("packed mip tail", nullptr, [&] { mipped(tail_base, tail_mips, 32, 16, true, 1); });
      std::printf("gfx_render_tests: packed mip tail: %u of %u pixels from level 1\n", p.Count(blue), kSize * kSize);
      CHECK(p.Count(blue) == kSize * kSize);
    }
    // A signed texture: k_8_8_8_8 with every component signed (sign 1), texels 0x40 0xC0 0x40 0x7F, read as
    // 64/127 = 0.504, -0.504, 0.504, 1 into a 16_16_16_16_FLOAT target.
    {
      const uint32_t signed_base = 0xC0000;
      for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
          SetBig(signed_base + uint32_t(vk::TiledOffset2D(int32_t(x), int32_t(y), 32, 2)), Rgba(0x40, 0xC0, 0x40, 0x7F));
        }
      }
      const vk::SurfaceKey target{1536, kSize, 7, 0};
      const float zero[4] = {0, 0, 0, 0};
      CHECK(renderer.ClearColor(target, zero));
      scenario("signed texture", nullptr, [&] {
        s.Shader(0, t::BuildTexturedVertexShader());
        s.Shader(1, t::BuildTexturedPixelShader(1));
        s.Reg(0x2001, 7u << 16 | 1536);
        s.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, quad | 3, 2u | 24u << 2});
        s.Put({Type3(pm4::SET_CONSTANT, 7), 1u << 16 | 6, 2u | 0x55u << 2 | 2u << 10 | 2u << 13 | 1u << 22 | 1u << 31,
               6u | 2u << 6 | signed_base, 31u | 31u << 13, 0x688u << 1, 0, 1u << 9});
        s.DrawAuto(kTriangleStrip, 4);
      });
      std::vector<uint8_t> bytes;
      CHECK(renderer.ReadBack(target, kSize, kSize, bytes));
      auto half = [](uint16_t h) {
        const uint32_t e = (h >> 10) & 0x1F;
        const float m = std::ldexp(float((h & 0x3FF) | (e ? 0x400 : 0)), (e ? int(e) : 1) - 25);
        return h & 0x8000 ? -m : m;
      };
      uint32_t right = 0;
      float first[4] = {};
      for (size_t i = 0; i + 8 <= bytes.size(); i += 8) {
        uint16_t h[4];
        std::memcpy(h, bytes.data() + i, 8);
        if (!i) for (int k = 0; k < 4; ++k) first[k] = half(h[k]);
        right += std::fabs(half(h[0]) - 64 / 127.0f) < 0.002f && std::fabs(half(h[1]) + 64 / 127.0f) < 0.002f &&
                 std::fabs(half(h[2]) - 64 / 127.0f) < 0.002f && std::fabs(half(h[3]) - 1.0f) < 0.002f;
      }
      std::printf("gfx_render_tests: signed texture: %u of %u pixels right (%.3f %.3f %.3f %.3f)\n", right, kSize * kSize,
                  first[0], first[1], first[2], first[3]);
      CHECK(right == kSize * kSize);
      s.Reg(0x2001, 0);
    }
    // Resolve: the quadrant texture (tiled k_8_8_8_8 at 0x60000) drawn into colour 0, resolved to guest memory at
    // 0x80000 (k_8_8_8_8, 8-in-32, tiled, pitch 64) with a green clear of the target, then sampled back from there.
    const uint32_t rect = kVertices + 0x800, resolved = 0x80000;
    const float rv[3][2] = {{0, 0}, {float(kSize), 0}, {float(kSize), float(kSize)}};
    for (uint32_t i = 0; i < 3; ++i) {
      SetBig(rect + 8 * i, Bits(rv[i][0]));
      SetBig(rect + 8 * i + 4, Bits(rv[i][1]));
    }
    std::memset(g_memory + resolved, 0xCD, 64 * 64 * 4);
    auto textured = [&](uint32_t base, uint32_t size) {
      s.Shader(0, t::BuildTexturedVertexShader());
      s.Shader(1, t::BuildTexturedPixelShader(1));
      s.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, quad | 3, 2u | 24u << 2});
      s.Put({Type3(pm4::SET_CONSTANT, 7), 1u << 16 | 6, 2u | 2u << 10 | 2u << 13 | (size / 32) << 22 | 1u << 31,
             6u | 2u << 6 | base, (size - 1) | (size - 1) << 13, 0x688u << 1, 0, 1u << 9});
      s.DrawAuto(kTriangleStrip, 4);
    };
    const uint32_t green = Rgba(0, 255, 0, 255);
    Pixels p = scenario("resolve", nullptr, [&] {
      textured(0x60000, 32);
      s.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, rect | 3, 2u | 6u << 2});  // vf0: the copy rectangle
      // Control (colour 0, clear colour and depth), base, pitch, info.
      s.Put({Type0(0x2318, 4), 0u | 1u << 8 | 1u << 9, resolved, 64u | 64u << 16, 2u | 6u << 7});
      s.Reg(0x231E, 0xFF00FF00);  // RB_COLOR_CLEAR: green, opaque (component 0 in the low byte)
      s.Reg(0x231D, 0);           // RB_DEPTH_CLEAR: depth 0 (nearest), stencil 0
      s.Reg(0x2208, 6);           // EDRAM copy mode
      s.DrawAuto(8, 3);           // the rectangle list the runtime draws for a resolve
      s.Reg(0x2208, 4);
      // The depth clear took: a quad at depth 0.5 tested LESS draws nothing over depth 0.
      s.Shader(0, t::BuildFetchPositionShader());
      s.Shader(1, t::BuildSingleExportShader(uc::AllocType::kPsColors, uint32_t(uc::ExportRegister::kPSColor0), {0, false}));
      s.Reg(kRbDepthControl, 1u << 1 | 1u << 2 | 1u << 4);
      s.Vertices(0);
      s.Color(255, 0, 0, 255);
      s.DrawAuto(kTriangleStrip, 4);
    });
    std::printf("gfx_render_tests: resolve: target after clear %u of %u green\n", p.Count(green), kSize * kSize);
    CHECK(p.Count(green) == kSize * kSize);
    uint32_t wrong_texels = 0;
    for (uint32_t y = 0; y < kSize; ++y) {
      for (uint32_t x = 0; x < kSize; ++x) {
        uint32_t big;
        std::memcpy(&big, g_memory + resolved + vk::TiledOffset2D(int32_t(x), int32_t(y), 64, 2), 4);
        wrong_texels += std::byteswap(big) != quadrant[quadrant_of(x, y, 32)];
      }
    }
    std::printf("gfx_render_tests: resolve: %u of %u texels wrong in guest memory\n", wrong_texels, kSize * kSize);
    CHECK(wrong_texels == 0);
    p = scenario("resolved texture", nullptr, [&] { textured(resolved, 64); });
    uint32_t wrong = 0;
    for (uint32_t y = 0; y < kSize; ++y) {
      for (uint32_t x = 0; x < kSize; ++x) wrong += p.At(x, y) != quadrant[quadrant_of(x, y, 32)];
    }
    std::printf("gfx_render_tests: resolve: sampled back, %u of %u pixels wrong\n", wrong, kSize * kSize);
    CHECK(wrong == 0);
    // Swap: what VdSwap writes, the front buffer's fetch constant 0 then XE_SWAP; the presented frame is the
    // resolved image.
    s.Put({Type0(0x4800, 6), 2u | 2u << 10 | 2u << 13 | 2u << 22 | 1u << 31, 6u | 2u << 6 | resolved, 63u | 63u << 13,
           0x688u << 1, 0, 1u << 9});
    s.Put({Type3(pm4::XE_SWAP, 4), 0x53574150, resolved, kSize, kSize});
    s.Run();
    const vk::VulkanRenderer::Frame& frame = renderer.frame();
    std::vector<uint32_t> frame_pixels;
    CHECK(renderer.ReadFrame(frame_pixels));
    uint32_t frame_wrong = frame_pixels.size() == kSize * kSize ? 0 : kSize * kSize;
    for (uint32_t y = 0; y < kSize && !frame_wrong; ++y) {
      for (uint32_t x = 0; x < kSize; ++x) frame_wrong += frame_pixels[y * kSize + x] != quadrant[quadrant_of(x, y, 32)];
    }
    std::printf("gfx_render_tests: swap: frame held %s\n", frame.on_gpu ? "on the GPU (the resolve's source)" : "on the CPU");
    std::printf("gfx_render_tests: swap: frame %llu, %ux%u, %u pixels wrong\n", (unsigned long long)frame.number, frame.width,
                frame.height, frame_wrong);
    CHECK(frame.number == 1 && frame.width == kSize && frame.height == kSize && frame_wrong == 0);
    // The window path: the GPU frame through the gamma ramp (an inverting ramp here) by the compute pass, blitted
    // into an image the size of the frame, read back: the same pixels as ReadFrame gives through the ramp on the CPU.
    {
      uint32_t table[256];
      for (uint32_t i = 0; i < 256; ++i) {
        const uint32_t v = 0x3FF - i * 0x3FF / 0xFF;
        table[i] = v | v << 10 | v << 20;
      }
      renderer.GammaRamp(table, nullptr);
      std::vector<uint32_t> through_cpu;
      CHECK(renderer.ReadFrame(through_cpu) && through_cpu.size() == kSize * kSize);
      vk::Image target;
      vk::Buffer readback;
      CHECK(vk::CreateImage2D(device, kSize, kSize, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, target));
      CHECK(vk::CreateBuffer(device, kSize * kSize * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback));
      const auto& f = device.fn();
      auto barrier = [&](VkCommandBuffer cb, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = target.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        f.vkCmdPipelineBarrier2(cb, &dep);
      };
      CHECK(device.Submit([&](VkCommandBuffer cb) {
        barrier(cb, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        renderer.RecordFrameBlit(cb, target.image, {0, 0, 0}, {int32_t(kSize), int32_t(kSize), 1});
        barrier(cb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kSize, kSize, 1};
        f.vkCmdCopyImageToBuffer(cb, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
      }));
      uint32_t differ = through_cpu.size() == kSize * kSize ? 0 : kSize * kSize;
      const uint32_t* through_gpu = static_cast<const uint32_t*>(readback.mapped);
      for (size_t i = 0; i < through_cpu.size() && !differ; ++i) {
        differ += (through_gpu[i] & 0xFFFFFF) != (through_cpu[i] & 0xFFFFFF);
      }
      std::printf("gfx_render_tests: window path through the gamma ramp: %u of %u pixels differ from ReadFrame (first %08X)\n", differ,
                  kSize * kSize, through_gpu[0]);
      CHECK(differ == 0 && (through_gpu[0] & 0xFFFFFF) != (quadrant[0] & 0xFFFFFF));
      vk::DestroyImage(device, target);
      vk::DestroyBuffer(device, readback);
      for (uint32_t i = 0; i < 256; ++i) {
        const uint32_t v = i * 0x3FF / 0xFF;
        table[i] = v | v << 10 | v << 20;
      }
      renderer.GammaRamp(table, nullptr);
    }
    // The same front buffer swizzled blue-green-red: no longer the resolve as it is, so read from guest memory on the
    // CPU, with red and blue exchanged.
    s.Put({Type0(0x4800, 6), 2u | 2u << 10 | 2u << 13 | 2u << 22 | 1u << 31, 6u | 2u << 6 | resolved, 63u | 63u << 13,
           (2u | 1u << 3 | 0u << 6 | 3u << 9) << 1, 0, 1u << 9});
    s.Put({Type3(pm4::XE_SWAP, 4), 0x53574150, resolved, kSize, kSize});
    s.Run();
    CHECK(renderer.ReadFrame(frame_pixels) && !renderer.frame().on_gpu && renderer.frame().number == 2);
    uint32_t swizzled_wrong = frame_pixels.size() == kSize * kSize ? 0 : kSize * kSize;
    for (uint32_t y = 0; y < kSize && !swizzled_wrong; ++y) {
      for (uint32_t x = 0; x < kSize; ++x) {
        const uint32_t q = quadrant[quadrant_of(x, y, 32)];
        swizzled_wrong += frame_pixels[y * kSize + x] != ((q & 0xFF00FF00u) | (q >> 16 & 0xFF) | (q & 0xFF) << 16);
      }
    }
    std::printf("gfx_render_tests: swap: swizzled front buffer read on the CPU, %u pixels wrong\n", swizzled_wrong);
    CHECK(swizzled_wrong == 0);
    s.Shader(0, t::BuildFetchPositionShader());
  }

  const vk::RendererStats& stats = renderer.stats();
  std::printf("gfx_render_tests: %s (depth %s), %u draws, %u skipped, %u failed%s%s, %u shaders, %u pipelines, %u render targets\n",
              device.properties().deviceName, renderer.depth_format() == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
              stats.draws, stats.skipped, stats.failed, stats.failed ? ": " : "", renderer.last_error().c_str(),
              stats.translations, stats.pipelines, stats.render_targets);
  // Render targets: colour 0, depth, colour 1, the 16_16_16_16_FLOAT target, the float24 depth surface and the nine
  // render target formats, of which the two AS_ ones share their base format's surface (12, not 14).
  CHECK(stats.failed == 0 && stats.skipped == 0 && stats.render_targets == 12 && stats.texture_uploads == 6 && stats.resolves == 1);
  // (The resolved texture is copied from the resolve on the GPU, not uploaded: 6 uploads, not 7.)
  // Depth resolve: a quad at depth 0.5 written into the D24S8 surface, its depth resolved to guest memory as k_24_8
  // (depth << 8 | stencil, 8-in-32, tiled), then sampled back as a k_24_8 texture into a 16_16_16_16_FLOAT target.
  // The float24 conversions round-trip first.
  {
    uint32_t round_trip_wrong = 0;
    for (float v : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.0f / 3.0f, 0.001f}) {
      round_trip_wrong += std::fabs(vk::Float20e4ToFloat(vk::FloatTo20e4(v)) - v) > v * 1e-6f;
    }
    CHECK(vk::FloatTo20e4(1.0f) == 0xF00000 && vk::FloatTo20e4(0.5f) == 0xE00000 && round_trip_wrong == 0);
    const uint32_t rect = kVertices + 0x800, quad = kVertices + 0x400, resolved = 0x90000;
    std::memset(g_memory + resolved, 0xCD, 64 * 64 * 4);
    scenario("depth resolve", nullptr, [&] {
      s.Shader(0, t::BuildFetchPositionShader());
      s.Shader(1, t::BuildSingleExportShader(uc::AllocType::kPsColors, uint32_t(uc::ExportRegister::kPSColor0), {0, false}));
      s.Reg(kRbDepthControl, 1u << 1 | 1u << 2 | 7u << 4);  // z_enable, z_write, ALWAYS
      s.Vertices(0);
      s.Color(255, 255, 255, 255);
      s.DrawAuto(kTriangleStrip, 4);
      s.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, rect | 3, 2u | 6u << 2});
      s.Put({Type0(0x2318, 4), 4u, resolved, 64u | 64u << 16, 2u | 22u << 7});  // depth, k_24_8, 8-in-32
      s.Reg(0x2208, 6);
      s.DrawAuto(8, 3);
      s.Reg(0x2208, 4);
    });
    uint32_t wrong_words = 0, sample = 0;
    for (uint32_t y = 0; y < kSize; ++y) {
      for (uint32_t x = 0; x < kSize; ++x) {
        uint32_t big;
        std::memcpy(&big, g_memory + resolved + vk::TiledOffset2D(int32_t(x), int32_t(y), 64, 2), 4);
        const uint32_t word = std::byteswap(big);
        if (x == 10 && y == 10) sample = word;
        wrong_words += (word & 0xFF) != 0 || std::abs(int32_t(word >> 8) - 0x800000) > 2;
      }
    }
    std::printf("gfx_render_tests: depth resolve: %u of %u words wrong (%08X)\n", wrong_words, kSize * kSize, sample);
    CHECK(wrong_words == 0);
    const vk::SurfaceKey target{1536, kSize, 7, 0};
    const float zero[4] = {0, 0, 0, 0};
    CHECK(renderer.ClearColor(target, zero));
    scenario("depth texture", nullptr, [&] {
      s.Shader(0, t::BuildTexturedVertexShader());
      s.Shader(1, t::BuildTexturedPixelShader(1));
      s.Reg(0x2001, 7u << 16 | 1536);
      s.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, quad | 3, 2u | 24u << 2});
      s.Put({Type3(pm4::SET_CONSTANT, 7), 1u << 16 | 6, 2u | 2u << 10 | 2u << 13 | 2u << 22 | 1u << 31,
             22u | 2u << 6 | resolved, 63u | 63u << 13, 0x688u << 1, 0, 1u << 9});
      s.DrawAuto(kTriangleStrip, 4);
    });
    std::vector<uint8_t> bytes;
    CHECK(renderer.ReadBack(target, kSize, kSize, bytes));
    uint32_t half_wrong = bytes.size() == kSize * kSize * 8 ? 0 : kSize * kSize;
    for (size_t i = 0; i + 8 <= bytes.size(); i += 8) {
      uint16_t h;
      std::memcpy(&h, bytes.data() + i, 2);
      half_wrong += h != 0x3800;  // 0.5
    }
    std::printf("gfx_render_tests: depth texture (k_24_8): %u of %u pixels not 0.5\n", half_wrong, kSize * kSize);
    CHECK(half_wrong == 0);
    s.Shader(0, t::BuildFetchPositionShader());
  }
  // MSAA resolves: the lower-left triangle drawn at 1x over the 64x64 samples, then resolved as a 4x surface of
  // pitch 32 (32x32 pixels, 2x2 samples each: top-left, bottom-left, top-right, bottom-right) three times, to
  // k_8_8_8_8 at pitch 32: all four samples averaged, the bottom-left sample alone, the top-right sample alone.
  // A pixel left of the diagonal (x < y) is the triangle's colour, right of it black; a diagonal pixel has its
  // bottom-left sample inside, its top-right one outside, and its other two (on the edge) on the same side.
  {
    const uint32_t rect = kVertices + 0x900, dests[3] = {0xD0000, 0xD1000, 0xD2000}, selects[3] = {6, 1, 2};
    const float rv[3][2] = {{0, 0}, {32, 0}, {32, 32}};
    for (uint32_t i = 0; i < 3; ++i) {
      SetBig(rect + 8 * i, Bits(rv[i][0]));
      SetBig(rect + 8 * i + 4, Bits(rv[i][1]));
    }
    for (uint32_t d : dests) std::memset(g_memory + d, 0xCD, 32 * 32 * 4);
    const uint32_t a = Rgba(200, 100, 0, 255);
    scenario("MSAA resolves", nullptr, [&] {
      s.Shader(0, t::BuildFetchPositionShader());
      s.Shader(1, t::BuildSingleExportShader(uc::AllocType::kPsColors, uint32_t(uc::ExportRegister::kPSColor0), {0, false}));
      s.Vertices(0);
      s.Color(200, 100, 0, 255);
      s.DrawAuto(kTriangleList, 3);
      s.Reg(0x2000, 32u | 2u << 16);  // RB_SURFACE_INFO: pitch 32, 4x MSAA
      s.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, rect | 3, 2u | 6u << 2});
      for (uint32_t i = 0; i < 3; ++i) {
        s.Put({Type0(0x2318, 4), selects[i] << 4, dests[i], 32u | 32u << 16, 2u | 6u << 7});  // colour 0, k_8_8_8_8
        s.Reg(0x2208, 6);
        s.DrawAuto(8, 3);
        s.Reg(0x2208, 4);
      }
    });
    uint32_t wrong[3] = {}, diagonal = 0;
    for (uint32_t y = 0; y < 32; ++y) {
      for (uint32_t x = 0; x < 32; ++x) {
        uint32_t texel[3];
        for (uint32_t i = 0; i < 3; ++i) {
          uint32_t big;
          std::memcpy(&big, g_memory + dests[i] + vk::TiledOffset2D(int32_t(x), int32_t(y), 32, 2), 4);
          texel[i] = std::byteswap(big);
        }
        if (x == y) {
          // One or three of four samples inside: 200/4 or 600/4 red (rounded), 100/4 or 300/4 green.
          wrong[0] += texel[0] != Rgba(50, 25, 0, 255) && texel[0] != Rgba(150, 75, 0, 255);
          if (x == 5) diagonal = texel[0];
        } else {
          wrong[0] += texel[0] != (x < y ? a : kBlack);
        }
        wrong[1] += texel[1] != (x <= y ? a : kBlack);
        wrong[2] += texel[2] != (x < y ? a : kBlack);
      }
    }
    std::printf("gfx_render_tests: MSAA resolves: %u / %u / %u of 1024 texels wrong (averaged, sample 1, sample 2), "
                "diagonal %08X\n", wrong[0], wrong[1], wrong[2], diagonal);
    CHECK(wrong[0] == 0 && wrong[1] == 0 && wrong[2] == 0);
  }
  // Capture and replay: a frame (two triangles, a resolve, a swap) captured as it runs, then replayed into fresh
  // memory, a fresh command processor and a fresh renderer; the replayed frame must be the captured one.
  {
    const uint32_t rect = kVertices + 0x800, resolved = 0x80000;
    const std::string path = g_capture.empty() ? "gfx_render_tests_capture.a360cap" : g_capture;
    capture::Writer writer;
    CHECK(writer.Open(path, g_memory, kMemory, cp.registers(), reg::kRegisterCount, "gfx_render_tests"));
    writer.Shaders(cp.shader(0).microcode, cp.shader(1).microcode);
    writer.GammaRamp(cp.gamma_table(), cp.gamma_pwl());
    cp.SetCapture(&writer);
    s.Defaults();
    s.Shader(1, t::BuildSingleExportShader(uc::AllocType::kPsColors, uint32_t(uc::ExportRegister::kPSColor0), {0, false}));
    s.Vertices(0);
    s.Color(51, 102, 153, 255);
    s.DrawAuto(kTriangleList, 3);
    s.Color(204, 51, 102, 255);
    s.DrawIndexed16(kTriangleList, 3);
    s.Run();  // a kick of its own: the replay must carry the state across kicks
    s.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, rect | 3, 2u | 6u << 2});
    s.Put({Type0(0x2318, 4), 0u, resolved, 64u | 64u << 16, 2u | 6u << 7});
    s.Reg(0x2208, 6);
    s.DrawAuto(8, 3);
    s.Reg(0x2208, 4);
    s.Put({Type0(0x4800, 6), 2u | 2u << 10 | 2u << 13 | 2u << 22 | 1u << 31, 6u | 2u << 6 | resolved, 63u | 63u << 13,
           0x688u << 1, 0, 1u << 9});
    s.Put({Type3(pm4::XE_SWAP, 4), 0x53574150, resolved, kSize, kSize});
    s.Run();
    cp.SetCapture(nullptr);
    std::vector<uint32_t> reference;
    CHECK(renderer.ReadFrame(reference) && reference.size() == kSize * kSize);
    if (reference.size() == kSize * kSize) writer.Frame(kSize, kSize, reference.data());
    const uint64_t capture_bytes = writer.bytes_written();
    writer.Close();
    const uint32_t a = Rgba(51, 102, 153, 255), b = Rgba(204, 51, 102, 255);
    uint32_t reference_ab = 0;
    for (uint32_t px : reference) reference_ab += px == a || px == b;
    CHECK(reference_ab == kSize * kSize);

    auto* fresh = static_cast<uint8_t*>(::operator new(kMemory, std::align_val_t(65536)));
    std::memset(fresh, 0, kMemory);
    {
      vk::VulkanRenderer replay_renderer(device, fresh, kMemory);
      CHECK(replay_renderer.Initialize());
      CommandProcessor replay(fresh);
      replay.SetRenderer(&replay_renderer);
      replay.SetInterruptHandler([](uint32_t, uint32_t) {});
      capture::ReplayStats rs;
      std::string error;
      uint32_t compared = 0, wrong = kSize * kSize;
      const bool ok = capture::Replay(path, replay, fresh, kMemory, [&](uint32_t w, uint32_t h, const uint32_t* rgba) {
        std::vector<uint32_t> ours;
        if (!replay_renderer.ReadFrame(ours) || w != kSize || h != kSize || ours.size() != size_t(w) * h) return;
        ++compared;
        wrong = 0;
        for (size_t i = 0; i < ours.size(); ++i) wrong += ours[i] != rgba[i];
      }, rs, error);
      CHECK(ok);
      std::printf("gfx_render_tests: capture: %llu KB, %u kicks, %u pages, %u frame; replay: %u draws, %u failed, %u of %u pixels "
                  "differ from the reference%s%s\n",
                  (unsigned long long)(capture_bytes / 1024), rs.kicks, rs.pages, rs.frames, replay_renderer.stats().draws,
                  replay_renderer.stats().failed, wrong, kSize * kSize, ok ? "" : ": ", error.c_str());
      CHECK(rs.kicks == 2 && rs.failed_kicks == 0 && rs.frames == 1 && compared == 1 && wrong == 0);
      CHECK(replay_renderer.stats().failed == 0 && replay_renderer.stats().resolves == 1);
      CHECK(replay.opcode_counts()[pm4::XE_SWAP] == 1 && replay.opcode_counts()[pm4::DRAW_INDX] == 1);
    }
    ::operator delete(fresh, std::align_val_t(65536));
    if (g_capture.empty()) std::remove(path.c_str());
  }
  // ALU opcodes: each operation of the microcode's vector and scalar units run by a pixel shader on constants c0-c3,
  // its result exported to a k_32_32_FLOAT target over the whole target and read back, against the semantics
  // ucode.h documents for it. A pixel killed keeps the clear value.
  {
    using V = uc::AluVectorOpcode;
    using S = uc::AluScalarOpcode;
    const uint32_t kColor0 = uint32_t(uc::ExportRegister::kPSColor0);
    const vk::SurfaceKey target{1536, kSize, 15, 0};
    constexpr float kMaxFloat = 3.402823466e+38f, kKilled = -9.0f;
    struct Case {
      std::string unit;
      std::vector<t::Alu> alus;
      float c[4][4];
      float x, y, tolerance;
      std::vector<uint32_t> ps = {}, vs = {};  // whole shaders instead of `alus` (and the default vertex shader)
      uint32_t bools = 0, loop0 = 0;          // bool constants 0-31 and loop constant 0
      std::function<void()> setup = {};       // after the default vertices: other vertices, textures
    };
    std::vector<Case> cases;
    const t::AluOperand c0{0, false}, c1{1, false}, c2{2, false}, c3{3, false};
    // A vector operation on c0, c1 (and c2), x and y exported.
    auto vector = [&](V op, std::initializer_list<float> a, std::initializer_list<float> b, std::initializer_list<float> c,
                      float x, float y, float tolerance = 0.0f) {
      Case k{std::string("shader ALU vector:") , {}, {}, x, y, tolerance};
      t::Alu alu;
      alu.vector = op, alu.vector_mask = 0b0011, alu.dest = kColor0, alu.is_export = true;
      alu.src1 = c0, alu.src2 = c1, alu.src3 = c2;
      k.alus = {alu};
      std::copy(a.begin(), a.end(), k.c[0]);
      std::copy(b.begin(), b.end(), k.c[1]);
      std::copy(c.begin(), c.end(), k.c[2]);
      cases.push_back(k);
      return &cases.back();
    };
    // A scalar operation on c2 (a = c2.w, b = c2.x), x and y exported.
    auto scalar = [&](S op, float a, float b, float result, float tolerance = 0.0f) {
      Case k{"shader ALU scalar:", {}, {}, result, result, tolerance};
      t::Alu alu;
      alu.scalar = op, alu.scalar_mask = 0b0011, alu.dest = kColor0, alu.is_export = true, alu.src3 = c2;
      k.alus = {alu};
      k.c[2][0] = b, k.c[2][3] = a;
      cases.push_back(k);
      return &cases.back();
    };
    // A scalar operation with the previous scalar result: first ps = c2.w + c2.x, then `op` on c3 (a = w, b = x).
    auto previous = [&](S op, float first_a, float first_b, float a, float b, float result) {
      Case k{"shader ALU scalar:", {}, {}, result, result, 0.0f};
      t::Alu first, second;
      first.scalar = S::kAdds, first.src3 = c2, first.dest = 1, first.scalar_dest = 1;
      second.scalar = op, second.scalar_mask = 0b0011, second.dest = kColor0, second.is_export = true, second.src3 = c3;
      k.alus = {first, second};
      k.c[2][0] = first_b, k.c[2][3] = first_a, k.c[3][0] = b, k.c[3][3] = a;
      cases.push_back(k);
      return &cases.back();
    };
    // A constant-scalar operation: r<temp> = c3 first (temp 0 or 1), then `op` on c2.w and r<temp>.x (the temp
    // register's index in the opcode's low bit, src3's selector and its swizzle bits).
    auto constant = [&](S op, uint32_t temp, float constant_w, float temp_x, float result) {
      Case k{"shader ALU scalar:", {}, {}, result, result, 0.0f};
      t::Alu load, alu;
      load.vector_mask = 0b1111, load.dest = temp, load.src1 = c3, load.src2 = c3;
      alu.scalar = S(uint32_t(op) | temp), alu.scalar_mask = 0b0011, alu.dest = kColor0, alu.is_export = true;
      alu.src3 = c2;
      k.alus = {load, alu};
      k.c[2][3] = constant_w, k.c[3][0] = temp_x;
      cases.push_back(k);
      return &cases.back();
    };
    const char* vnames[] = {"kAdd", "kMul", "kMax", "kMin", "kSeq", "kSgt", "kSge", "kSne", "kFrc", "kTrunc",
                            "kFloor", "kMad", "kCndEq", "kCndGe", "kCndGt", "kDp4", "kDp3", "kDp2Add", "kCube",
                            "kMax4", "kSetpEqPush", "kSetpNePush", "kSetpGtPush", "kSetpGePush", "kKillEq",
                            "kKillGt", "kKillGe", "kKillNe", "kDst", "kMaxA"};
    auto vname = [&](Case* k, V op) { k->unit += vnames[uint32_t(op)]; };
    vname(vector(V::kAdd, {1.5f, -2.25f}, {0.25f, 4}, {}, 1.75f, 1.75f), V::kAdd);
    vname(vector(V::kMul, {1.5f, -2}, {4, 0.5f}, {}, 6, -1), V::kMul);
    vname(vector(V::kMax, {1, 5}, {3, 2}, {}, 3, 5), V::kMax);
    vname(vector(V::kMin, {1, 5}, {3, 2}, {}, 1, 2), V::kMin);
    vname(vector(V::kSeq, {1, 2}, {1, 3}, {}, 1, 0), V::kSeq);
    vname(vector(V::kSgt, {2, 1}, {1, 1}, {}, 1, 0), V::kSgt);
    vname(vector(V::kSge, {1, 0}, {1, 1}, {}, 1, 0), V::kSge);
    vname(vector(V::kSne, {1, 2}, {1, 3}, {}, 0, 1), V::kSne);
    vname(vector(V::kFrc, {1.25f, -1.25f}, {}, {}, 0.25f, 0.75f), V::kFrc);
    vname(vector(V::kTrunc, {1.75f, -1.75f}, {}, {}, 1, -1), V::kTrunc);
    vname(vector(V::kFloor, {1.75f, -1.25f}, {}, {}, 1, -2), V::kFloor);
    vname(vector(V::kMad, {2, 3}, {4, -1}, {0.5f, 0.5f}, 8.5f, -2.5f), V::kMad);
    vname(vector(V::kCndEq, {0, 1}, {5, 5}, {7, 7}, 5, 7), V::kCndEq);
    vname(vector(V::kCndGe, {0, -1}, {5, 5}, {7, 7}, 5, 7), V::kCndGe);
    vname(vector(V::kCndGt, {0, 1}, {5, 5}, {7, 7}, 7, 5), V::kCndGt);
    vname(vector(V::kDp4, {1, 2, 3, 4}, {5, 6, 7, 8}, {}, 70, 70), V::kDp4);
    vname(vector(V::kDp3, {1, 2, 3, 4}, {5, 6, 7, 8}, {}, 38, 38), V::kDp3);
    vname(vector(V::kDp2Add, {1, 2, 3, 4}, {5, 6, 7, 8}, {0.5f, 9}, 17.5f, 17.5f), V::kDp2Add);
    vname(vector(V::kMax4, {1, 9, 3, 4}, {}, {}, 9, 9), V::kMax4);
    // cube r, s.zzxy, s.yxzz: x = T, y = S of the face of the major axis (z here, then x).
    auto cube = [&](float sx, float sy, float sz, float t_coord, float s_coord) {
      Case* k = vector(V::kCube, {sx, sy, sz, 0}, {sx, sy, sz, 0}, {}, t_coord, s_coord);
      k->alus[0].src1_swizzle = 2 | 1 << 2 | 2 << 4 | 2 << 6;  // zzxy
      k->alus[0].src2_swizzle = 1 | 3 << 2 | 0 << 4 | 3 << 6;  // yxzz
      vname(k, V::kCube);
    };
    cube(0.5f, 0.25f, 1, -0.25f, 0.5f);
    cube(-1, 0.5f, 0.25f, -0.5f, 0.25f);
    // setp_*_push: dest = 0 when src0.x == 0 and the comparison of src1.x holds, else src0.x + 1.
    vname(vector(V::kSetpEqPush, {0, 0, 0, 0}, {0, 0, 0, 0}, {}, 0, 0), V::kSetpEqPush);
    vname(vector(V::kSetpNePush, {2, 0, 0, 0}, {1, 0, 0, 0}, {}, 3, 3), V::kSetpNePush);
    vname(vector(V::kSetpGtPush, {0, 0, 0, 0}, {1, 0, 0, 0}, {}, 0, 0), V::kSetpGtPush);
    vname(vector(V::kSetpGePush, {0, 0, 0, 0}, {-1, 0, 0, 0}, {}, 1, 1), V::kSetpGePush);
    // kill_*: any component comparing true discards the pixel; none, dest = 0.
    vname(vector(V::kKillEq, {1, 2, 3, 4}, {9, 2, 9, 9}, {}, kKilled, kKilled), V::kKillEq);
    vname(vector(V::kKillGt, {1, 2, 3, 4}, {1, 2, 3, 4}, {}, 0, 0), V::kKillGt);
    vname(vector(V::kKillGe, {1, 2, 3, 4}, {1, 9, 9, 9}, {}, kKilled, kKilled), V::kKillGe);
    vname(vector(V::kKillNe, {1, 2, 3, 4}, {1, 2, 3, 4}, {}, 0, 0), V::kKillNe);
    vname(vector(V::kDst, {9, 2, 3, 9}, {9, 5, 9, 7}, {}, 1, 10), V::kDst);
    vname(vector(V::kMaxA, {1.5f, 2}, {1, 3}, {}, 1.5f, 3), V::kMaxA);
    const char* snames[] = {"kAdds", "kAddsPrev", "kMuls", "kMulsPrev", "kMulsPrev2", "kMaxs", "kMins", "kSeqs",
                            "kSgts", "kSges", "kSnes", "kFrcs", "kTruncs", "kFloors", "kExp", "kLogc", "kLog",
                            "kRcpc", "kRcpf", "kRcp", "kRsqc", "kRsqf", "kRsq", "kMaxAs", "kMaxAsf", "kSubs",
                            "kSubsPrev", "kSetpEq", "kSetpNe", "kSetpGt", "kSetpGe", "kSetpInv", "kSetpPop",
                            "kSetpClr", "kSetpRstr", "kKillsEq", "kKillsGt", "kKillsGe", "kKillsNe", "kKillsOne",
                            "kSqrt", "", "kMulsc0", "kMulsc1", "kAddsc0", "kAddsc1", "kSubsc0", "kSubsc1", "kSin",
                            "kCos", "kRetainPrev"};
    auto sname = [&](Case* k, S op) { k->unit += snames[uint32_t(op)]; };
    sname(scalar(S::kAdds, 1.5f, 2.25f, 3.75f), S::kAdds);
    sname(scalar(S::kMuls, 1.5f, -2, -3), S::kMuls);
    sname(scalar(S::kMaxs, 1, 3, 3), S::kMaxs);
    sname(scalar(S::kMins, 1, 3, 1), S::kMins);
    sname(scalar(S::kSeqs, 0, 5, 1), S::kSeqs);
    sname(scalar(S::kSgts, 0.5f, 0, 1), S::kSgts);
    sname(scalar(S::kSges, -0.5f, 0, 0), S::kSges);
    sname(scalar(S::kSnes, 0, 5, 0), S::kSnes);
    sname(scalar(S::kFrcs, -1.25f, 0, 0.75f), S::kFrcs);
    sname(scalar(S::kTruncs, -1.75f, 0, -1), S::kTruncs);
    sname(scalar(S::kFloors, -1.25f, 0, -2), S::kFloors);
    sname(scalar(S::kExp, 3, 0, 8, 1e-5f), S::kExp);
    sname(scalar(S::kLogc, 0, 0, -kMaxFloat), S::kLogc);
    sname(scalar(S::kLog, 8, 0, 3, 1e-5f), S::kLog);
    sname(scalar(S::kRcpc, 0, 0, kMaxFloat), S::kRcpc);
    sname(scalar(S::kRcpf, 0, 0, 0), S::kRcpf);
    sname(scalar(S::kRcp, 4, 0, 0.25f, 1e-6f), S::kRcp);
    sname(scalar(S::kRsqc, 0, 0, kMaxFloat), S::kRsqc);
    sname(scalar(S::kRsqf, 0, 0, 0), S::kRsqf);
    sname(scalar(S::kRsq, 4, 0, 0.5f, 1e-6f), S::kRsq);
    sname(scalar(S::kMaxAs, 5, 3, 5), S::kMaxAs);
    sname(scalar(S::kMaxAsf, 2, 7, 7), S::kMaxAsf);
    sname(scalar(S::kSubs, 5, 2, 3), S::kSubs);
    sname(scalar(S::kSetpEq, 0, 0, 0), S::kSetpEq);
    sname(scalar(S::kSetpNe, 0, 0, 1), S::kSetpNe);
    sname(scalar(S::kSetpGt, 2, 0, 0), S::kSetpGt);
    sname(scalar(S::kSetpGe, -2, 0, 1), S::kSetpGe);
    sname(scalar(S::kSetpInv, 3, 0, 3), S::kSetpInv);
    sname(scalar(S::kSetpPop, 3, 0, 2), S::kSetpPop);
    sname(scalar(S::kSetpClr, 3, 0, kMaxFloat), S::kSetpClr);
    sname(scalar(S::kSetpRstr, 3, 0, 3), S::kSetpRstr);
    sname(scalar(S::kKillsEq, 0, 0, kKilled), S::kKillsEq);
    sname(scalar(S::kKillsGt, 0, 0, 0), S::kKillsGt);
    sname(scalar(S::kKillsGe, 0, 0, kKilled), S::kKillsGe);
    sname(scalar(S::kKillsNe, 0, 0, 0), S::kKillsNe);
    sname(scalar(S::kKillsOne, 1, 0, kKilled), S::kKillsOne);
    sname(scalar(S::kSqrt, 9, 0, 3, 1e-6f), S::kSqrt);
    sname(scalar(S::kSin, 1.57079633f, 0, 1, 1e-4f), S::kSin);
    sname(scalar(S::kCos, 0, 0, 1, 1e-4f), S::kCos);
    sname(previous(S::kAddsPrev, 1, 2, 0.5f, 0, 3.5f), S::kAddsPrev);
    sname(previous(S::kMulsPrev, 1, 2, 4, 0, 12), S::kMulsPrev);
    sname(previous(S::kSubsPrev, 1, 2, 10, 0, 7), S::kSubsPrev);
    sname(previous(S::kMulsPrev2, 1, 2, 4, 1, 12), S::kMulsPrev2);
    sname(previous(S::kRetainPrev, 1, 2, 0, 0, 3), S::kRetainPrev);
    // Constant-scalar operations: both opcode numbers of each pair (the low bit chooses the temp register).
    for (uint32_t temp : {0u, 1u}) {
      sname(constant(S::kMulsc0, temp, 3, 4, 12), S(uint32_t(S::kMulsc0) | temp));
      sname(constant(S::kAddsc0, temp, 3, 4, 7), S(uint32_t(S::kAddsc0) | temp));
      sname(constant(S::kSubsc0, temp, 3, 4, -1), S(uint32_t(S::kSubsc0) | temp));
    }

    // Control flow: r1 = c0 (A), the flow under test may add c1 to it (B), then oC0 = r1 (C); E exports c1 instead,
    // P sets the predicate from c2.w == 0. c0 = (1, 2), c1 = (10, 20): (1, 2) when B is skipped, (11, 22) when it
    // runs once.
    {
      using O = uc::ControlFlowOpcode;
      t::Alu a, b, c, e, p;
      a.vector_mask = 0b1111, a.dest = 1, a.src1 = c0, a.src2 = c0;
      b.vector = V::kAdd, b.vector_mask = 0b1111, b.dest = 1, b.src1 = {1, true}, b.src2 = c1;
      c.vector_mask = 0b0011, c.dest = kColor0, c.is_export = true, c.src1 = c.src2 = {1, true};
      e.vector_mask = 0b0011, e.dest = kColor0, e.is_export = true, e.src1 = e.src2 = c1;
      p.scalar = S::kSetpEq, p.src3 = c2, p.dest = 2, p.scalar_dest = 2;
      // Instruction slots after four control flow pairs: A 4, P 5, B 6, C 7, E 8.
      constexpr uint32_t kA = 4, kB = 6, kC = 7, kE = 8;
      std::vector<uint32_t> body;
      for (const t::Alu* alu : {&a, &p, &b, &c, &e}) t::PushAlu(body, *alu);
      auto flow = [&](const char* unit, std::vector<t::Cf> cf, uint32_t bools, uint32_t loop0, float p_source, float x,
                      float y) {
        cf.insert(cf.begin(), t::CfAlloc(uc::AllocType::kPsColors, 0));
        while (cf.size() < 8) cf.push_back(t::CfNop());
        Case k{std::string("shader control flow:") + unit, {}, {}, x, y, 0.0f};
        k.ps = t::BuildCfShader(cf, body);
        k.bools = bools, k.loop0 = loop0;
        k.c[0][0] = 1, k.c[0][1] = 2, k.c[1][0] = 10, k.c[1][1] = 20, k.c[2][3] = p_source;
        cases.push_back(k);
      };
      const t::Cf exec_a = t::CfExec(O::kExec, kA, 1, 0), exec_ap = t::CfExec(O::kExec, kA, 2, 0),
                  exec_b = t::CfExec(O::kExec, kB, 1, 0), end_c = t::CfExec(O::kExecEnd, kC, 1, 0);
      flow("kNop", {t::CfNop(), exec_a, end_c}, 0, 0, 1, 1, 2);
      for (bool taken : {true, false}) {
        const float x = taken ? 11 : 1, y = taken ? 22 : 2, ex = taken ? 10 : 1, ey = taken ? 20 : 2;
        flow("kCondExec", {exec_a, t::CfCondExec(O::kCondExec, kB, 1, 0, 3, true), end_c}, taken ? 8 : 0, 0, 1, x, y);
        flow("kCondExecEnd", {exec_a, t::CfCondExec(O::kCondExecEnd, kE, 1, 0, 3, true), end_c}, taken ? 8 : 0, 0, 1,
             ex, ey);
        flow("kCondExecPredClean", {exec_a, t::CfCondExec(O::kCondExecPredClean, kB, 1, 0, 3, true), end_c},
             taken ? 8 : 0, 0, 1, x, y);
        flow("kCondExecPredCleanEnd", {exec_a, t::CfCondExec(O::kCondExecPredCleanEnd, kE, 1, 0, 3, true), end_c},
             taken ? 8 : 0, 0, 1, ex, ey);
        // The predicate: p0 = (c2.w == 0).
        flow("kCondExecPred", {exec_ap, t::CfCondExecPred(O::kCondExecPred, kB, 1, 0, true), end_c}, 0, 0,
             taken ? 0.0f : 5.0f, x, y);
        flow("kCondExecPredEnd", {exec_ap, t::CfCondExecPred(O::kCondExecPredEnd, kE, 1, 0, true), end_c}, 0, 0,
             taken ? 0.0f : 5.0f, ex, ey);
        // A call to B (control flow 4) and its return, unconditional or on bool 3; C after it.
        for (const char* unit : {"kCondCall", "kReturn"}) {
          flow(unit, {exec_a, t::CfBranch(O::kCondCall, 4, taken, 3, true), end_c, exec_b, t::CfReturn()}, 0, 0, 1,
               taken ? 11 : 1, taken ? 22 : 2);
        }
        // A jump over B: unconditional (B skipped) or on bool 3 being set (clear here: B runs).
        flow("kCondJmp", {exec_a, t::CfBranch(O::kCondJmp, 4, taken, 3, true), exec_b, end_c}, 0, 0, 1,
             taken ? 1 : 11, taken ? 2 : 22);
      }
      // A loop of B, three iterations (loop constant 0: count 3, start 0, step 1): r1 = c0 + 3 c1. The loop start
      // jumps past its end (control flow 5) when the count is 0; the end back to the body (control flow 3).
      for (const char* unit : {"kLoopStart", "kLoopEnd"}) {
        flow(unit, {exec_a, t::CfLoopStart(5, 0), exec_b, t::CfLoopEnd(3, 0), end_c}, 0, 3u | 0u << 8 | 1u << 16, 1,
             31, 62);
      }
      // kMarkVsFetchDone in a vertex shader between its fetch and its position export.
      {
        Case k{"shader control flow:kMarkVsFetchDone", {}, {}, 1, 2, 0.0f};
        t::Alu position;
        position.vector_mask = 0b1111, position.dest = uint32_t(uc::ExportRegister::kVSPosition), position.is_export = true;
        position.src1 = position.src2 = {1, true};
        std::vector<uint32_t> vs_body;
        t::PushVertexFetch(vs_body, 1, 0, 0, rex::graphics::xenos::VertexFormat::k_32_32_32_32_FLOAT, 4, 0);
        t::PushAlu(vs_body, position);
        k.vs = t::BuildCfShader({t::CfAlloc(uc::AllocType::kVsPosition, 0), t::CfExec(O::kExec, 2, 1, 0b01),
                                 t::CfMarkVsFetchDone(), t::CfExec(O::kExecEnd, 3, 1, 0)},
                                vs_body);
        t::Alu export_c0;
        export_c0.vector_mask = 0b0011, export_c0.dest = kColor0, export_c0.is_export = true, export_c0.src1 = export_c0.src2 = c0;
        k.alus = {export_c0};
        k.c[0][0] = 1, k.c[0][1] = 2;
        cases.push_back(k);
      }
    }

    // The texture unit's queries and setters, on a full-target quad whose texcoords (u right, v down, 0 to 1) reach
    // the pixel shader in r0: a 128x128 texture with two levels (red, then green at 64x64) and a 32x32 one (four
    // coloured quadrants). Read at pixel (32, 32).
    {
      using F = uc::FetchOpcode;
      const uint32_t quad = kVertices + 0x400, mip_base = 0xA0000, mip_levels = 0xB0000, small = 0x50000;
      const float v[4][6] = {{-1, -1, 0, 1, 0, 1}, {1, -1, 0, 1, 1, 1}, {-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}};
      for (uint32_t i = 0; i < 4; ++i) {
        for (uint32_t c = 0; c < 6; ++c) SetBig(quad + 24 * i + 4 * c, Bits(v[i][c]));
      }
      for (uint32_t y = 0; y < 128; ++y) {
        for (uint32_t x = 0; x < 128; ++x) {
          SetBig(mip_base + uint32_t(vk::TiledOffset2D(int32_t(x), int32_t(y), 128, 2)), Rgba(255, 0, 0, 255));
          if (x < 64 && y < 64) SetBig(mip_levels + uint32_t(vk::TiledOffset2D(int32_t(x), int32_t(y), 64, 2)), Rgba(0, 255, 0, 255));
          if (x < 32 && y < 32) SetBig(small + y * 256 + x * 4, Rgba(x < 16 ? 255 : 0, y < 16 ? 255 : 0, 0, 255));
        }
      }
      // tf1: the two-level texture (tiled, point filters, levels 0-1), or the small one (linear, one level).
      auto bind = [&s, quad, mip_base, mip_levels, small](bool mipped) {
        s.Shader(0, t::BuildTexturedVertexShader());
        s.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, quad | 3, 2u | 24u << 2});
        if (mipped) {
          s.Put({Type3(pm4::SET_CONSTANT, 7), 1u << 16 | 6, 2u | 2u << 10 | 2u << 13 | 4u << 22 | 1u << 31,
                 6u | 2u << 6 | mip_base, 127u | 127u << 13, 0x688u << 1, 1u << 6, 1u << 9 | mip_levels});
        } else {
          s.Put({Type3(pm4::SET_CONSTANT, 7), 1u << 16 | 6, 2u | 2u << 10 | 2u << 13 | 1u << 22, 6u | 2u << 6 | small,
                 31u | 31u << 13, 0x688u << 1, 0, 1u << 9});
        }
      };
      t::Alu export_r1;
      export_r1.vector_mask = 0b0011, export_r1.dest = kColor0, export_r1.is_export = true;
      export_r1.src1 = export_r1.src2 = {1, true};
      t::Alu load_r2, load_r3;
      load_r2.vector_mask = 0b1111, load_r2.dest = 2, load_r2.src1 = load_r2.src2 = c0;
      load_r3.vector_mask = 0b1111, load_r3.dest = 3, load_r3.src1 = load_r3.src2 = c1;
      // `program`: the instructions in order, `fetches` a bit for each that is a fetch; then r1.xy exported.
      auto fetch_case = [&](const char* unit, bool mipped, std::vector<std::function<void(std::vector<uint32_t>&)>> program,
                            uint32_t fetches, std::initializer_list<float> c0_value, std::initializer_list<float> c1_value,
                            float x, float y, float tolerance) {
        std::vector<uint32_t> words;
        for (auto& push : program) push(words);
        t::PushAlu(words, export_r1);
        const uint32_t count = uint32_t(program.size()) + 1;
        uint32_t sequence = 0;
        for (uint32_t i = 0; i < count; ++i) sequence |= ((fetches >> i) & 1) << (2 * i);
        Case k{std::string("shader fetch:") + unit, {}, {}, x, y, tolerance};
        k.ps = t::BuildCfShader({t::CfAlloc(uc::AllocType::kPsColors, 0),
                                 t::CfExec(uc::ControlFlowOpcode::kExecEnd, 1, count, sequence)},
                                words);
        std::copy(c0_value.begin(), c0_value.end(), k.c[0]);
        std::copy(c1_value.begin(), c1_value.end(), k.c[1]);
        k.setup = [bind, mipped] { bind(mipped); };
        cases.push_back(k);
      };
      auto tex = [](t::Tex x) { return [x](std::vector<uint32_t>& w) { t::PushTex(w, x); }; };
      auto alu = [](t::Alu a) { return [a](std::vector<uint32_t>& w) { t::PushAlu(w, a); }; };
      constexpr uint32_t kXYKeep = 7u << 6 | 7u << 9;
      t::Tex gradients;
      gradients.opcode = F::kGetTextureGradients, gradients.src_swizzle = 0 | 1 << 2;
      fetch_case("kGetTextureGradients", false, {tex(gradients)}, 0b1, {}, {}, 1.0f / 64, 0, 1e-4f);  // ddx(u), ddy(u)
      gradients.dest_swizzle = 2 | 3 << 3 | kXYKeep;
      fetch_case("kGetTextureGradients", false, {tex(gradients)}, 0b1, {}, {}, 0, 1.0f / 64, 1e-4f);  // ddx(v), ddy(v)
      t::Tex lod;
      lod.opcode = F::kGetTextureComputedLod, lod.dest_swizzle = 0 | 0 << 3 | kXYKeep;
      fetch_case("kGetTextureComputedLod", true, {tex(lod)}, 0b1, {}, {}, 1, 1, 1e-2f);  // 128 texels over 64 pixels
      // setTexLOD r2.x, then a fetch at the register LOD (point mip filter): level 1 (green) or 0 (red).
      t::Tex set_lod, at_lod;
      set_lod.opcode = F::kSetTextureLod, set_lod.src = 2, set_lod.src_swizzle = 0;
      at_lod.computed_lod = false, at_lod.register_lod = true, at_lod.mip = 0;
      for (float level : {1.0f, 0.0f}) {
        fetch_case("kSetTextureLod", true, {alu(load_r2), tex(set_lod), tex(at_lod)}, 0b110, {level}, {},
                   level ? 0.0f : 1.0f, level ? 1.0f : 0.0f, 0.0f);
      }
      // setTexGradientsH r2.xyz, setTexGradientsV r3.xyz, then a fetch with the register gradients: two texels a
      // pixel (level 1, green) or half a texel (level 0, red).
      t::Tex set_h, set_v, at_gradients;
      set_h.opcode = F::kSetTextureGradientsHorz, set_h.src = 2;
      set_v.opcode = F::kSetTextureGradientsVert, set_v.src = 3;
      at_gradients.register_gradients = true, at_gradients.mip = 0;
      for (float texels : {2.0f, 0.5f}) {
        for (const char* unit : {"kSetTextureGradientsHorz", "kSetTextureGradientsVert"}) {
          fetch_case(unit, true, {alu(load_r2), alu(load_r3), tex(set_h), tex(set_v), tex(at_gradients)}, 0b11100,
                     {texels / 128, 0, 0}, {0, texels / 128, 0}, texels > 1 ? 0.0f : 1.0f, texels > 1 ? 1.0f : 0.0f,
                     0.0f);
        }
      }
      // Bilinear weights on the 32x32 texture magnified twice: D3D pixel centres are at integer coordinates, so pixel
      // 32 reads u = 0.5, texel coordinate 16 - 0.5: weights 0.5 (the translator adds a 1.5/1024 rounding offset).
      t::Tex weights;
      weights.opcode = F::kGetTextureWeights, weights.mag = weights.min = 1, weights.mip = 0;
      weights.dest_swizzle = 0 | 1 << 3 | kXYKeep;
      fetch_case("kGetTextureWeights", false, {tex(weights)}, 0b1, {}, {}, 0.5f, 0.5f, 1.0f / 256);
      // kGetTextureBorderColorFrac: not tested, the translator does not implement it (a translation error).
    }

    // Vertex formats: a full-target quad whose vertices carry, after their float4 position, four dwords of an
    // attribute in the format under test (the same in every vertex, big-endian, swapped 8-in-32 by the fetch
    // constant); the vertex shader fetches it into interpolator 0, the pixel shader exports it. x and y, then z and
    // w through the fetch's destination swizzle. Components the format lacks read 0 (a title's shader asks for 1
    // through the swizzle, which the XNA compiler emits as the constant-1 selector).
    {
      using VF = rex::graphics::xenos::VertexFormat;
      const uint32_t buffer = kVertices + 0x800;
      const char* names[64] = {};
      names[6] = "k_8_8_8_8", names[7] = "k_2_10_10_10", names[16] = "k_10_11_11", names[17] = "k_11_11_10",
      names[25] = "k_16_16", names[26] = "k_16_16_16_16", names[31] = "k_16_16_FLOAT", names[32] = "k_16_16_16_16_FLOAT",
      names[33] = "k_32", names[34] = "k_32_32", names[35] = "k_32_32_32_32", names[36] = "k_32_FLOAT",
      names[37] = "k_32_32_FLOAT", names[38] = "k_32_32_32_32_FLOAT", names[57] = "k_32_32_32_FLOAT";
      t::Alu export_r0;
      export_r0.vector_mask = 0b0011, export_r0.dest = kColor0, export_r0.is_export = true;
      export_r0.src1 = export_r0.src2 = {0, true};
      auto vertex_case = [&](VF format, bool is_signed, bool is_integer, int32_t exp_adjust,
                             std::array<uint32_t, 4> attribute, std::array<float, 4> expected, float tolerance) {
        for (bool zw : {false, true}) {
          t::VertexFetch position, value;
          position.stride_dwords = value.stride_dwords = 8;
          value.dest = 2, value.format = format, value.offset_dwords = 4, value.exp_adjust = exp_adjust;
          value.is_signed = is_signed, value.is_integer = is_integer;
          if (zw) value.dest_swizzle = 2 | 3 << 3 | 7 << 6 | 7 << 9;
          std::vector<uint32_t> words;
          t::PushVertexFetch(words, position);
          t::PushVertexFetch(words, value);
          t::Alu out_position, out_value;
          out_position.vector_mask = out_value.vector_mask = 0b1111;
          out_position.is_export = out_value.is_export = true;
          out_position.dest = uint32_t(uc::ExportRegister::kVSPosition);
          out_value.dest = uint32_t(uc::ExportRegister::kVSInterpolator0);
          out_position.src1 = out_position.src2 = {1, true};
          out_value.src1 = out_value.src2 = {2, true};
          t::PushAlu(words, out_position);
          t::PushAlu(words, out_value);
          Case k{std::string("vertex formats:") + names[uint32_t(format)], {export_r0}, {},
                 expected[zw ? 2 : 0], expected[zw ? 3 : 1], tolerance};
          k.vs = t::BuildCfShader({t::CfAlloc(uc::AllocType::kVsPosition, 0),
                                   t::CfExec(uc::ControlFlowOpcode::kExec, 2, 3, 0b000101),
                                   t::CfAlloc(uc::AllocType::kVsInterpolators, 0),
                                   t::CfExec(uc::ControlFlowOpcode::kExecEnd, 5, 1, 0)},
                                  words);
          k.setup = [&s, buffer, attribute] {
            const float corners[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
            for (uint32_t i = 0; i < 4; ++i) {
              const float position[4] = {corners[i][0], corners[i][1], 0.5f, 1};
              for (uint32_t c = 0; c < 4; ++c) SetBig(buffer + 32 * i + 4 * c, Bits(position[c]));
              for (uint32_t c = 0; c < 4; ++c) SetBig(buffer + 32 * i + 16 + 4 * c, attribute[c]);
            }
            s.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, buffer | 3, 2u | 32u << 2});
          };
          cases.push_back(k);
        }
      };
      // Half floats: 0.5 = 0x3800, -2 = 0xC000, 1024 = 0x6400, 0.125 = 0x3000.
      const float u8 = 1.0f / 255, s8 = 1.0f / 127;
      vertex_case(VF::k_8_8_8_8, false, false, 0, {0x80C04020}, {32 * u8, 64 * u8, 192 * u8, 128 * u8}, 1e-6f);
      vertex_case(VF::k_8_8_8_8, true, false, 0, {0x7FC0E020}, {32 * s8, -32 * s8, -64 * s8, 1}, 1e-6f);
      vertex_case(VF::k_8_8_8_8, false, true, 0, {0x80C04020}, {32, 64, 192, 128}, 0.0f);
      vertex_case(VF::k_2_10_10_10, false, false, 0, {100u | 1000u << 10 | 512u << 20 | 3u << 30},
                  {100.0f / 1023, 1000.0f / 1023, 512.0f / 1023, 1}, 1e-6f);
      vertex_case(VF::k_10_11_11, false, false, 0, {1000u | 2000u << 11 | 500u << 22},
                  {1000.0f / 2047, 2000.0f / 2047, 500.0f / 1023, 0}, 1e-6f);
      vertex_case(VF::k_11_11_10, false, false, 0, {500u | 1000u << 10 | 2000u << 21},
                  {500.0f / 1023, 1000.0f / 2047, 2000.0f / 2047, 0}, 1e-6f);
      vertex_case(VF::k_16_16, false, false, 0, {0xC0004000}, {16384.0f / 65535, 49152.0f / 65535, 0, 0}, 1e-6f);
      vertex_case(VF::k_16_16, true, false, 0, {0xC0004000}, {16384.0f / 32767, -16384.0f / 32767, 0, 0}, 1e-6f);
      vertex_case(VF::k_16_16, false, true, -1, {0x00080004}, {2, 4, 0, 0}, 0.0f);  // integer, times 2^-1
      vertex_case(VF::k_16_16_16_16, false, true, 0, {1u | 2u << 16, 3u | 4u << 16}, {1, 2, 3, 4}, 0.0f);
      vertex_case(VF::k_16_16_FLOAT, false, false, 0, {0xC0003800}, {0.5f, -2, 0, 0}, 0.0f);
      vertex_case(VF::k_16_16_16_16_FLOAT, false, false, 0, {0xC0003800, 0x30006400},
                  {0.5f, -2, 1024, 0.125f}, 0.0f);
      vertex_case(VF::k_32, false, true, 0, {1234}, {1234, 0, 0, 0}, 0.0f);
      vertex_case(VF::k_32_32, true, true, 0, {uint32_t(-5), 7}, {-5, 7, 0, 0}, 0.0f);
      vertex_case(VF::k_32_32_32_32, false, true, 0, {1, 2, 3, 4}, {1, 2, 3, 4}, 0.0f);
      vertex_case(VF::k_32_FLOAT, false, false, 0, {Bits(2.5f)}, {2.5f, 0, 0, 0}, 0.0f);
      vertex_case(VF::k_32_32_FLOAT, false, false, 0, {Bits(2.5f), Bits(-3)}, {2.5f, -3, 0, 0}, 0.0f);
      vertex_case(VF::k_32_32_32_FLOAT, false, false, 0, {Bits(1.5f), Bits(2.5f), Bits(3.5f)}, {1.5f, 2.5f, 3.5f, 0}, 0.0f);
      vertex_case(VF::k_32_32_32_32_FLOAT, false, false, 0, {Bits(1), Bits(-2), Bits(3), Bits(-4)}, {1, -2, 3, -4}, 0.0f);
    }

    const float clear[4] = {kKilled, kKilled, 0, 0};
    std::vector<std::string> passed;
    for (const Case& k : cases) {
      const uint32_t failed_before = renderer.stats().failed;
      CHECK(renderer.ClearColor(target, clear));
      s.Defaults();
      s.Reg(0x2001, 15u << 16 | 1536);  // RB_COLOR_INFO: k_32_32_FLOAT at tile 1536
      s.Shader(1, k.ps.empty() ? t::BuildAluPixelShader(k.alus) : k.ps);
      if (!k.vs.empty()) s.Shader(0, k.vs);
      s.Reg(0x4900, k.bools);  // SHADER_CONSTANT_BOOL_000_031
      s.Reg(0x4908, k.loop0);  // SHADER_CONSTANT_LOOP_00
      s.Put({Type3(pm4::SET_CONSTANT, 17), 0u << 16 | 1024});
      for (uint32_t i = 0; i < 16; ++i) s.Put(Bits(k.c[i / 4][i % 4]));
      s.Vertices(0);
      if (k.setup) k.setup();
      s.DrawAuto(kTriangleStrip, 4);
      if (!k.vs.empty() || k.setup) s.Shader(0, t::BuildFetchPositionShader());
      s.Run();
      std::vector<uint8_t> bytes;
      CHECK(renderer.ReadBack(target, kSize, kSize, bytes));
      float got[2] = {};
      if (bytes.size() == size_t(kSize) * kSize * 8) std::memcpy(got, bytes.data() + (32 * kSize + 32) * 8, 8);
      auto near = [&](float g, float e) {
        return g == e || std::fabs(g - e) <= k.tolerance * std::max(1.0f, std::fabs(e));
      };
      const bool ok = renderer.stats().failed == failed_before && near(got[0], k.x) && near(got[1], k.y);
      if (ok) {
        passed.push_back(k.unit);
      } else {
        std::printf("gfx_render_tests: %s: got (%g, %g), expected (%g, %g)%s%s\n", k.unit.c_str(), got[0], got[1], k.x,
                    k.y, renderer.stats().failed != failed_before ? ": " : "",
                    renderer.stats().failed != failed_before ? renderer.last_error().c_str() : "");        ++g_failures;
      }
    }
    std::printf("gfx_render_tests: shader opcodes: %zu of %zu cases right\n", passed.size(), cases.size());
    s.Shader(1, t::BuildSingleExportShader(uc::AllocType::kPsColors, uint32_t(uc::ExportRegister::kPSColor0), {0, false}));
  }
  std::printf("gfx_render_tests: guest memory %s\n", renderer.guest_memory_in_place() ? "read in place" : "copied per draw");
  std::printf("gfx_render_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
