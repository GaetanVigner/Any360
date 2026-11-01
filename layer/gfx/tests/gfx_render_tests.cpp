// End to end on a real driver: PM4 written by hand (no game data) runs through the command processor, whose draws
// the Vulkan renderer turns into host draws with translated shaders, vertices fetched from guest memory, constants
// from the register file and indices by DMA; the render state comes from the registers. Each scenario clears the
// targets, runs its packets and checks the pixels it read back. Skipped without a Vulkan 1.3 device.
#include <bit>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

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
std::vector<uint8_t> g_physical(kMemory);

// Register indices the scenarios write.
constexpr uint32_t kRbDepthControl = 0x2200, kRbBlendControl0 = 0x2201, kRbColorMask = 0x2104,
                   kRbStencilRefMask = 0x210D, kPaSuScModeCntl = 0x2205, kPaScWindowScissorTl = 0x2081,
                   kPaScWindowScissorBr = 0x2082;
// Xenos primitive types.
constexpr uint32_t kTriangleList = 4, kTriangleStrip = 6;

void SetBig(uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(g_physical.data() + address, &value, 4);
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
    Reg(0x2208, 4);
    Reg(0x2206, 0x3F | 1u << 10);
    Put({Type0(0x210F, 6), Bits(kSize / 2.0f), Bits(kSize / 2.0f), Bits(-float(kSize) / 2.0f), Bits(kSize / 2.0f), Bits(1), Bits(0)});
    Put({Type0(0x200E, 2), 0, kSize << 16 | kSize});
    Put({Type0(0x2080, 3), 0, 0, kSize << 16 | kSize});
    Reg(kRbDepthControl, 0);
    Reg(kRbBlendControl0, 0x00010001);
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
  if (argc > 1) options.prefer = argv[1];
  if (!device.Create(options)) {
    std::printf("gfx_render_tests: skipped (%s)\n", device.error().c_str());
    return 0;
  }
  vk::VulkanRenderer renderer(device, g_physical.data(), kMemory);
  CHECK(renderer.Initialize(kSize, kSize));
  CommandProcessor cp(g_physical.data());
  cp.SetRenderer(&renderer);
  cp.InitializeRingBuffer({}, kRing, 12);  // 32 KB: 8192 dwords
  Scene s{cp};

  WriteQuad(0, 0.5f);
  WriteQuad(1, 0.75f);
  WriteQuad(2, 0.25f);
  const uint16_t indices[3] = {1, 3, 2};  // the upper-right triangle of a quad
  for (uint32_t i = 0; i < 3; ++i) {
    const uint16_t big = uint16_t(indices[i] << 8 | indices[i] >> 8);
    std::memcpy(g_physical.data() + kIndices + 2 * i, &big, 2);
  }
  s.Shader(0, t::BuildFetchPositionShader());
  s.Shader(1, t::BuildSingleExportShader(uc::AllocType::kPsColors, uint32_t(uc::ExportRegister::kPSColor0), {0, false}));

  const float black[4] = {0, 0, 0, 1};
  auto scenario = [&](const char* name, const float* clear, const std::function<void()>& packets) {
    const uint32_t failed_before = renderer.stats().failed;
    CHECK(renderer.Clear(clear ? clear : black));
    CHECK(renderer.ClearDepthStencil(1.0f, 0));
    s.Defaults();
    packets();
    s.Run();
    if (renderer.stats().failed != failed_before) std::printf("gfx_render_tests: %s: %s\n", name, renderer.last_error().c_str());
    Pixels p;
    CHECK(renderer.ReadBack(p.data));
    if (p.data.size() != kSize * kSize) p.data.assign(kSize * kSize, 0);
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

  const vk::RendererStats& stats = renderer.stats();
  std::printf("gfx_render_tests: %s (depth %s), %u draws, %u skipped, %u failed%s%s, %u shaders, %u pipelines\n",
              device.properties().deviceName, renderer.depth_format() == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
              stats.draws, stats.skipped, stats.failed, stats.failed ? ": " : "", renderer.last_error().c_str(),
              stats.translations, stats.pipelines);
  CHECK(stats.failed == 0 && stats.skipped == 0 && stats.translations == 2);
  std::printf("gfx_render_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
