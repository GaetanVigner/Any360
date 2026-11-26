// What the renderer costs and keeps over a long run, on a real driver, with no game data: thousands of frames of
// hand-written PM4 (textured draws whose textures change and come and go, a resolve into guest memory, a swap) through
// the command processor and the Vulkan renderer. Checked: nothing the renderer holds grows without bound (textures
// are evicted once unused, render targets, resolves and pipelines stay as many as the frames use), the process's
// memory stops growing once warm, no draw fails. Reported, as the baseline optimizations are measured against: CPU
// and GPU milliseconds a frame, the process's memory, what the renderer holds.
//
// gfx_resource_tests [--frames N] [GPU name]: 3000 frames by default (eviction takes textures unused for 1200).
// Skipped without a Vulkan 1.3 device.
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "any360/gfx/command_processor.h"
#include "any360/gfx/vulkan/renderer.h"
#include "ucode_encode.h"

// After Any360's headers: windows.h's macros (Yield) would rename their functions.
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace any360::gfx;
using pm4::Type0;
using pm4::Type3;
namespace t = any360::gfx::test;
namespace uc = rex::graphics::ucode;

int g_failures = 0;
#define CHECK(condition)                                          \
  do {                                                            \
    if (!(condition)) {                                           \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

// Guest memory: ring, vertices, the resolve's destination, then 32x32 k_8_8_8_8 textures (4 KB each, tiled).
constexpr uint32_t kMemory = 0x1000000, kRing = 0x10000, kQuad = 0x40000, kRect = 0x40400, kResolved = 0x80000,
                   kTextures = 0x100000, kTextureBytes = 0x1000, kSize = 64;
// Each frame draws kDraws textured quads: kSteady textures always the same (one of them rewritten every frame), and
// one from a window sliding over kPassing others (each used for a while, then never again: eviction must free them).
constexpr uint32_t kDraws = 8, kSteady = kDraws - 1, kPassing = 2048, kPassingFrames = 1;
uint8_t* g_memory = nullptr;

void SetBig(uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(g_memory + address, &value, 4);
}
uint32_t Bits(float f) { return std::bit_cast<uint32_t>(f); }

struct Ring {
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
  bool Run() {
    cp.WriteRegister(reg::CP_RB_WPTR, write);
    return cp.Execute();
  }
};

// The process's committed private memory (Windows) or resident set (elsewhere), in bytes.
uint64_t ProcessBytes() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
    return 0;
  }
  return counters.PrivateUsage;
#else
  long pages = 0, resident = 0;
  if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
    if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
    std::fclose(f);
  }
  return uint64_t(resident) * uint64_t(sysconf(_SC_PAGESIZE));
#endif
}

struct Sample {
  uint32_t frame = 0;
  uint64_t process_bytes = 0;
  size_t textures = 0, surfaces = 0, resolved = 0, pipelines = 0;
  double cpu_ms = 0;  // wall time per frame on the submitting thread since the previous sample
  double gpu_ms = 0;  // the GPU's time running batches per frame since the previous sample
};

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  uint32_t frames = 3000;
  vk::DeviceOptions options;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = uint32_t(std::max(400, std::atoi(argv[++i])));
    else options.prefer = argv[i];
  }
  {
    // Read-back texels to RGBA8 (screenshots, ReadFrame): 10-bit front buffers (The Simpsons Game) rounded per channel,
    // both channel orders; BGRA8 swapped; RGBA8 kept.
    std::vector<uint32_t> p = {0x3FF | 0x200u << 10 | 0x000u << 20 | 3u << 30, 0x001 | 0x002u << 10 | 0x3FEu << 20 | 1u << 30};
    vk::VulkanRenderer::TexelsToRGBA8(VK_FORMAT_A2B10G10R10_UNORM_PACK32, p);
    CHECK(p[0] == (0xFFu | 0x80u << 8 | 0x00u << 16 | 0xFFu << 24));  // 0x200 * 255 / 1023 rounds to 128
    CHECK(p[1] == (0x00u | 0x00u << 8 | 0xFFu << 16 | 0x55u << 24));
    std::vector<uint32_t> q = {0x3FF | 0x000u << 10 | 0x155u << 20 | 2u << 30};
    vk::VulkanRenderer::TexelsToRGBA8(VK_FORMAT_A2R10G10B10_UNORM_PACK32, q);
    CHECK(q[0] == (0x55u | 0x00u << 8 | 0xFFu << 16 | 0xAAu << 24));
    std::vector<uint32_t> b = {0x11223344}, r = {0x11223344};
    vk::VulkanRenderer::TexelsToRGBA8(VK_FORMAT_B8G8R8A8_UNORM, b);
    vk::VulkanRenderer::TexelsToRGBA8(VK_FORMAT_R8G8B8A8_UNORM, r);
    CHECK(b[0] == 0x11443322 && r[0] == 0x11223344);
  }
  vk::Device device;
  if (!device.Create(options)) {
    std::printf("gfx_resource_tests: skipped (%s)\n", device.error().c_str());
    return g_failures ? 1 : 0;
  }
  g_memory = static_cast<uint8_t*>(::operator new(kMemory, std::align_val_t(65536)));
  std::memset(g_memory, 0, kMemory);
  {
    vk::VulkanRenderer renderer(device, g_memory, kMemory);
    CHECK(renderer.Initialize());
    CommandProcessor cp(g_memory);
    cp.SetRenderer(&renderer);
    cp.InitializeRingBuffer({}, kRing, 12);
    Ring r{cp};

    // A full-target quad of 6-float vertices (position xyzw, texcoord uv) in strip order, and the resolve's rectangle.
    const float quad[4][6] = {{-1, -1, 0.5f, 1, 0, 1}, {1, -1, 0.5f, 1, 1, 1}, {-1, 1, 0.5f, 1, 0, 0}, {1, 1, 0.5f, 1, 1, 0}};
    for (uint32_t v = 0; v < 4; ++v) {
      for (uint32_t c = 0; c < 6; ++c) SetBig(kQuad + 24 * v + 4 * c, Bits(quad[v][c]));
    }
    const float rect[3][2] = {{0, 0}, {float(kSize), 0}, {0, float(kSize)}};
    for (uint32_t v = 0; v < 3; ++v) {
      for (uint32_t c = 0; c < 2; ++c) SetBig(kRect + 8 * v + 4 * c, Bits(rect[v][c]));
    }
    // Every texture a colour of its own.
    for (uint32_t i = 0; i < kSteady + kPassing; ++i) {
      for (uint32_t texel = 0; texel < kTextureBytes / 4; ++texel) {
        SetBig(kTextures + i * kTextureBytes + 4 * texel, 0xFF000000u | (i * 2654435761u & 0xFFFFFF));
      }
    }
    const std::vector<uint32_t> vs = t::BuildTexturedVertexShader(), ps = t::BuildTexturedPixelShader(1);
    auto frame = [&](uint32_t n) {
      // Surfaces: pitch 64, colour 0 at tile 0 (k_8_8_8_8), depth at tile 1024; colour + depth mode; full range
      // indices; a viewport over the target; no depth, blending or culling.
      r.Put({Type0(0x2000, 3), kSize, 0u << 16 | 0, 0u << 16 | 1024});
      r.Reg(0x2208, 4);
      r.Put({Type0(0x2100, 4), 0xFFFFFF, 0, 0, 0});
      r.Reg(0x2206, 0x3F | 1u << 10);
      r.Put({Type0(0x210F, 6), Bits(kSize / 2.0f), Bits(kSize / 2.0f), Bits(-float(kSize) / 2.0f), Bits(kSize / 2.0f),
             Bits(1), Bits(0)});
      r.Put({Type0(0x200E, 2), 0, kSize << 16 | kSize});
      r.Put({Type0(0x2080, 3), 0, 0, kSize << 16 | kSize});
      r.Reg(0x2200, 0);
      r.Reg(0x2201, 0x00010001);
      r.Reg(0x2104, 0xF);
      r.Reg(0x2205, 0);
      r.Shader(0, vs);
      r.Shader(1, ps);
      r.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, kQuad | 3, 2u | 24u << 2});
      // One steady texture rewritten every frame (an upload a frame).
      SetBig(kTextures, 0xFF000000u | n);
      for (uint32_t d = 0; d < kDraws; ++d) {
        const uint32_t index = d < kSteady ? d : kSteady + (n / kPassingFrames) % kPassing;
        const uint32_t base = kTextures + index * kTextureBytes;
        r.Put({Type3(pm4::SET_CONSTANT, 7), 1u << 16 | 6, 2u | 2u << 10 | 2u << 13 | 1u << 22 | 1u << 31,
               6u | 2u << 6 | base, 31u | 31u << 13, 0x688u << 1, 0, 1u << 9});
        r.Put({Type3(pm4::DRAW_INDX_2, 1), 4u << 16 | 2u << 6 | 6u});  // auto-indexed strip of 4
      }
      // Resolve colour 0 into guest memory (k_8_8_8_8, 8-in-32), as a frame's end does, then swap to it.
      r.Put({Type3(pm4::SET_CONSTANT, 3), 1u << 16 | 0, kRect | 3, 2u | 6u << 2});
      r.Put({Type0(0x2318, 4), 0u, kResolved, kSize | kSize << 16, 2u | 6u << 7});
      r.Reg(0x2208, 6);
      r.Put({Type3(pm4::DRAW_INDX_2, 1), 3u << 16 | 2u << 6 | 8u});
      r.Reg(0x2208, 4);
      r.Put({Type0(0x4800, 6), 2u | 2u << 10 | 2u << 13 | 2u << 22 | 1u << 31, 6u | 2u << 6 | kResolved,
             63u | 63u << 13, 0x688u << 1, 0, 1u << 9});
      r.Put({Type3(pm4::XE_SWAP, 4), 0x53574150, kResolved, kSize, kSize});
      return r.Run();
    };

    std::vector<Sample> samples;
    const uint32_t every = std::max(100u, frames / 10);
    auto start = std::chrono::steady_clock::now();
    uint64_t gpu_before = 0;
    uint32_t frame_before = 0;
    bool ran = true;
    for (uint32_t n = 1; n <= frames && ran; ++n) {
      ran = frame(n);
      if (n % every == 0 || n == frames) {
        renderer.Flush();
        renderer.WaitIdle();  // every batch done: the GPU time is all counted
        const auto now = std::chrono::steady_clock::now();
        Sample s;
        s.frame = n;
        s.process_bytes = ProcessBytes();
        s.textures = renderer.texture_cache()->texture_count();
        s.surfaces = renderer.surface_count();
        s.resolved = renderer.resolved_count();
        s.pipelines = renderer.pipeline_count();
        s.cpu_ms = std::chrono::duration<double, std::milli>(now - start).count() / double(n - frame_before);
        s.gpu_ms = double(renderer.stats().ns_gpu - gpu_before) / 1e6 / double(n - frame_before);
        samples.push_back(s);
        std::printf("gfx_resource_tests: frame %5u: %.3f ms CPU, %.3f ms GPU a frame; process %.1f MB; textures %zu, "
                    "render targets %zu, resolves %zu, pipelines %zu\n",
                    n, s.cpu_ms, s.gpu_ms, double(s.process_bytes) / (1 << 20), s.textures, s.surfaces, s.resolved,
                    s.pipelines);
        start = std::chrono::steady_clock::now();
        gpu_before = renderer.stats().ns_gpu;
        frame_before = n;
      }
    }
    CHECK(ran);
    CHECK(renderer.stats().failed == 0);
    if (renderer.stats().failed) std::printf("gfx_resource_tests: %s\n", renderer.last_error().c_str());
    // The frame presented is the last one's resolve: the steady texture's colour drawn last is a passing one's.
    std::vector<uint32_t> pixels;
    CHECK(renderer.ReadFrame(pixels) && pixels.size() == kSize * kSize);

    if (samples.size() >= 4) {
      const Sample& half = samples[samples.size() / 2];
      const Sample& last = samples.back();
      // Textures: the steady ones plus the passing ones used within the eviction's 1200 frames (one a frame), with
      // a frame or two of slack for when eviction runs.
      const size_t texture_bound = kSteady + std::min<size_t>(kPassing, 1200 / kPassingFrames + 2);
      std::printf("gfx_resource_tests: textures held %zu (bound %zu)\n", last.textures, texture_bound);
      CHECK(last.textures <= texture_bound);
      CHECK(last.surfaces == half.surfaces && last.resolved == half.resolved && last.pipelines == half.pipelines);
      // The process stops growing once warm: from the run's middle to its end, less than 16 MB more.
      const double growth = (double(last.process_bytes) - double(half.process_bytes)) / (1 << 20);
      std::printf("gfx_resource_tests: process memory from frame %u to %u: %+.1f MB\n", half.frame, last.frame, growth);
      CHECK(growth < 16.0);
    }
  }
  ::operator delete(g_memory, std::align_val_t(65536));
  std::printf("gfx_resource_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
