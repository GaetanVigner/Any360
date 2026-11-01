// End to end on a real driver: PM4 written by hand (no game data) runs through the command processor, whose draws
// the Vulkan renderer turns into host draws with translated shaders, vertices fetched from guest memory, constants
// from the register file and indices by DMA; the pixels are read back and checked. Skipped without a Vulkan 1.3
// device.
#include <bit>
#include <cstdio>
#include <cstring>
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

constexpr uint32_t kMemory = 0x200000, kRing = 0x10000, kVertices = 0x40000, kIndices = 0x41000;
constexpr uint32_t kSize = 64;
std::vector<uint8_t> g_physical(kMemory);

void SetBig(uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(g_physical.data() + address, &value, 4);
}
uint32_t Bits(float f) { return std::bit_cast<uint32_t>(f); }

struct Ring {
  CommandProcessor& cp;
  uint32_t write = 0;
  void Put(uint32_t dword) { SetBig(kRing + 4 * write++, dword); }
  void Put(std::initializer_list<uint32_t> dwords) {
    for (uint32_t d : dwords) Put(d);
  }
  void Shader(uint32_t type, const std::vector<uint32_t>& words) {
    Put({Type3(pm4::IM_LOAD_IMMEDIATE, 2 + uint32_t(words.size())), type, uint32_t(words.size())});
    for (uint32_t w : words) Put(w);
  }
  void PixelColor(float r, float g, float b, float a) {
    // Pixel shader c0 is float constant 256: dword 1024 of the ALU constants.
    Put({Type3(pm4::SET_CONSTANT, 5), 0u << 16 | 1024, Bits(r), Bits(g), Bits(b), Bits(a)});
  }
};

uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return uint32_t(a) << 24 | uint32_t(b) << 16 | uint32_t(g) << 8 | r; }

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
  const float clear[4] = {0, 0, 0, 1};
  CHECK(renderer.Clear(clear));
  CommandProcessor cp(g_physical.data());
  cp.SetRenderer(&renderer);
  cp.InitializeRingBuffer({}, kRing, 12);

  // Four vertices (float4, big-endian as the title writes them) and three 16-bit indices.
  const float vertices[4][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {-1, 1, 0, 1}, {1, 1, 0, 1}};
  for (uint32_t v = 0; v < 4; ++v) {
    for (uint32_t c = 0; c < 4; ++c) SetBig(kVertices + 16 * v + 4 * c, Bits(vertices[v][c]));
  }
  const uint16_t indices[3] = {1, 3, 2};
  for (uint32_t i = 0; i < 3; ++i) {
    const uint16_t big = uint16_t(indices[i] << 8 | indices[i] >> 8);
    std::memcpy(g_physical.data() + kIndices + 2 * i, &big, 2);
  }

  Ring ring{cp};
  // State: colour+depth EDRAM mode; a D3D viewport over the target (y up), W as is.
  ring.Put({Type0(0x2208, 1), 4});
  ring.Put({Type0(0x2206, 1), 0x3F | 1u << 10});
  ring.Put({Type0(0x210F, 6), Bits(kSize / 2.0f), Bits(kSize / 2.0f), Bits(-float(kSize) / 2.0f), Bits(kSize / 2.0f), Bits(1), Bits(0)});
  // Vertex fetch constant 0: the vertices (type 3, address in dwords), big-endian words, 16 words.
  ring.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, kVertices | 3, 2u | 16u << 2});
  ring.Shader(0, t::BuildFetchPositionShader());
  ring.Shader(1, t::BuildSingleExportShader(uc::AllocType::kPsColors, uint32_t(uc::ExportRegister::kPSColor0), {0, false}));
  // Draw 1: the lower-left triangle, auto-indexed, colour (0.2, 0.4, 0.6).
  ring.PixelColor(51 / 255.0f, 102 / 255.0f, 153 / 255.0f, 1);
  ring.Put({Type3(pm4::DRAW_INDX_2, 1), 3u << 16 | 2u << 6 | 4});
  // Draw 2: the upper-right triangle, 16-bit indices by DMA (swapped 8-in-16), colour (0.8, 0.2, 0.4).
  ring.PixelColor(204 / 255.0f, 51 / 255.0f, 102 / 255.0f, 1);
  ring.Put({Type3(pm4::DRAW_INDX, 4), 0, 3u << 16 | 0u << 6 | 4, kIndices, 1u << 30 | 3});
  cp.WriteRegister(reg::CP_RB_WPTR, ring.write);
  CHECK(cp.Execute());

  const vk::RendererStats& stats = renderer.stats();
  std::printf("gfx_render_tests: %s, %u draws, %u failed%s%s, %u shaders, %u pipelines\n", device.properties().deviceName,
              stats.draws, stats.failed, stats.failed ? ": " : "", renderer.last_error().c_str(), stats.translations,
              stats.pipelines);
  CHECK(stats.draws == 2 && stats.failed == 0 && stats.translations == 2 && stats.pipelines == 1);

  std::vector<uint32_t> pixels;
  CHECK(renderer.ReadBack(pixels));
  if (pixels.size() == kSize * kSize) {
    auto at = [&](uint32_t x, uint32_t y) { return pixels[y * kSize + x]; };
    const uint32_t first = Rgba(51, 102, 153, 255), second = Rgba(204, 51, 102, 255);
    std::printf("gfx_render_tests: lower-left %08X, upper-right %08X, centre-ish %08X\n", at(4, 59), at(59, 4), at(31, 31));
    // Draw 1 covers x + (63 - y) < 63 (the lower-left half, y down), draw 2 the rest.
    CHECK(at(4, 59) == first && at(2, 40) == first && at(20, 61) == first);
    CHECK(at(59, 4) == second && at(40, 2) == second && at(61, 20) == second);
    uint32_t first_count = 0, second_count = 0, other = 0;
    for (uint32_t p : pixels) (p == first ? first_count : p == second ? second_count : other)++;
    std::printf("gfx_render_tests: %u + %u pixels drawn, %u other\n", first_count, second_count, other);
    CHECK(other == 0 && first_count > 1900 && second_count > 1900);
  }
  std::printf("gfx_render_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
