// End to end on a real driver: PM4 written by hand (no game data) runs through the command processor, whose draws
// the Vulkan renderer turns into host draws with translated shaders, vertices fetched from guest memory, constants
// from the register file and indices by DMA; the render state comes from the registers. Each scenario clears the
// targets, runs its packets and checks the pixels it read back. Skipped without a Vulkan 1.3 device.
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
  CHECK(stats.failed == 0 && stats.skipped == 0 && stats.render_targets == 4 && stats.texture_uploads == 6 && stats.resolves == 1);
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
  std::printf("gfx_render_tests: guest memory %s\n", renderer.guest_memory_in_place() ? "read in place" : "copied per draw");
  std::printf("gfx_render_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
