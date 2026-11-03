// gfx_replay: replays a frame capture (capture.h) through any360::gfx's command processor and Vulkan renderer and
// reports what a real title's frames need: the draws the renderer cannot make yet and why, the PM4 packets the frames
// use, and how far each presented frame is from the reference's. Captures hold a title's data: they are made and
// replayed on the same machine, never committed.
//
//   gfx_replay CAPTURE [--gpu NAME] [--out DIR]
//     --gpu NAME  a substring of the device to use ("NVIDIA", "Radeon")
//     --out DIR   writes each compared frame as DIR/frame_N_ours.png and DIR/frame_N_reference.png
//     --trace     prints the frames' structure: runs of draws by render target (drawn, skipped, failed), each
//                 resolve and each swap, in order
//     --dump-resolves DIR  with --trace, writes each 32-bit resolve's destination as DIR/resolve_N.png (and depth
//                 resolves' words as DIR/resolve_N.bin)
//     --watch ADDR    with --trace, prints the texture fetch constants that read the 4 KB page at ADDR (hex)
//     --scale N   renders at N x N host pixels a guest pixel (resolution scale); frames are averaged down to the
//                 reference's size to be compared
//     --pipeline-cache FILE  keeps the pipelines in FILE across runs (the time spent creating them is printed)
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <bit>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "any360/gfx/async_renderer.h"
#include "any360/gfx/capture.h"
#include "any360/gfx/command_processor.h"
#include "any360/gfx/vulkan/renderer.h"

// After any360's headers: windows.h defines macros (Yield) that clash with their names.
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <timeapi.h>
#else
#include <sys/mman.h>
#endif

namespace {

using namespace any360::gfx;

constexpr uint64_t kPhysicalBytes = 512ull << 20;

// The guest's physical memory: zero pages, backed by the system as the replay touches them.
uint8_t* AllocatePhysical() {
#ifdef _WIN32
  return static_cast<uint8_t*>(VirtualAlloc(nullptr, kPhysicalBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
  void* p = mmap(nullptr, kPhysicalBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
}

const char* OpcodeName(uint32_t opcode) {
  switch (opcode) {
    case pm4::NOP: return "NOP";
    case pm4::REG_RMW: return "REG_RMW";
    case pm4::DRAW_INDX: return "DRAW_INDX";
    case pm4::VIZ_QUERY: return "VIZ_QUERY";
    case pm4::WAIT_FOR_IDLE: return "WAIT_FOR_IDLE";
    case pm4::IM_LOAD: return "IM_LOAD";
    case pm4::IM_LOAD_IMMEDIATE: return "IM_LOAD_IMMEDIATE";
    case pm4::SET_CONSTANT: return "SET_CONSTANT";
    case pm4::LOAD_ALU_CONSTANT: return "LOAD_ALU_CONSTANT";
    case pm4::DRAW_INDX_2: return "DRAW_INDX_2";
    case pm4::INDIRECT_BUFFER_PFD: return "INDIRECT_BUFFER_PFD";
    case pm4::INVALIDATE_STATE: return "INVALIDATE_STATE";
    case pm4::WAIT_REG_MEM: return "WAIT_REG_MEM";
    case pm4::MEM_WRITE: return "MEM_WRITE";
    case pm4::REG_TO_MEM: return "REG_TO_MEM";
    case pm4::INDIRECT_BUFFER: return "INDIRECT_BUFFER";
    case pm4::COND_WRITE: return "COND_WRITE";
    case pm4::EVENT_WRITE: return "EVENT_WRITE";
    case pm4::ME_INIT: return "ME_INIT";
    case pm4::SET_BIN_MASK: return "SET_BIN_MASK";
    case pm4::SET_BIN_SELECT: return "SET_BIN_SELECT";
    case pm4::INTERRUPT: return "INTERRUPT";
    case pm4::CONTEXT_UPDATE: return "CONTEXT_UPDATE";
    case pm4::SET_CONSTANT2: return "SET_CONSTANT2";
    case pm4::SET_SHADER_CONSTANTS: return "SET_SHADER_CONSTANTS";
    case pm4::EVENT_WRITE_SHD: return "EVENT_WRITE_SHD";
    case pm4::EVENT_WRITE_EXT: return "EVENT_WRITE_EXT";
    case pm4::EVENT_WRITE_ZPD: return "EVENT_WRITE_ZPD";
    case pm4::SET_BIN_MASK_LO: return "SET_BIN_MASK_LO";
    case pm4::SET_BIN_MASK_HI: return "SET_BIN_MASK_HI";
    case pm4::SET_BIN_SELECT_LO: return "SET_BIN_SELECT_LO";
    case pm4::SET_BIN_SELECT_HI: return "SET_BIN_SELECT_HI";
    case pm4::XE_SWAP: return "XE_SWAP";
    default: return nullptr;
  }
}

// A PNG of RGBA8 pixels (R in the low byte) in stored (uncompressed) deflate blocks: no compression library needed.
bool WritePng(const std::string& path, uint32_t width, uint32_t height, const uint32_t* rgba) {
  static uint32_t crc_table[256];
  if (!crc_table[1]) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      crc_table[n] = c;
    }
  }
  std::vector<uint8_t> raw;  // per row: filter byte 0, then the pixels
  raw.reserve(size_t(height) * (1 + size_t(width) * 4));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    const uint8_t* row = reinterpret_cast<const uint8_t*>(rgba + size_t(y) * width);
    raw.insert(raw.end(), row, row + size_t(width) * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  size_t at = 0;
  do {
    const uint16_t n = uint16_t(std::min<size_t>(65535, raw.size() - at));
    const bool last = at + n == raw.size();
    z.insert(z.end(), {uint8_t(last), uint8_t(n), uint8_t(n >> 8), uint8_t(~n), uint8_t(uint16_t(~n) >> 8)});
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
  } while (at < raw.size());
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  const uint32_t adler = b << 16 | a;
  z.insert(z.end(), {uint8_t(adler >> 24), uint8_t(adler >> 16), uint8_t(adler >> 8), uint8_t(adler)});

  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  auto be32 = [&](uint32_t v) {
    const uint8_t bytes[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    std::fwrite(bytes, 1, 4, f);
  };
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    be32(uint32_t(data.size()));
    uint32_t crc = ~0u;
    auto feed = [&](const uint8_t* p, size_t n) {
      for (size_t i = 0; i < n; ++i) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    };
    std::fwrite(type, 1, 4, f);
    feed(reinterpret_cast<const uint8_t*>(type), 4);
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    feed(data.data(), data.size());
    be32(~crc);
  };
  const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::fwrite(signature, 1, 8, f);
  chunk("IHDR", {uint8_t(width >> 24), uint8_t(width >> 16), uint8_t(width >> 8), uint8_t(width), uint8_t(height >> 24),
                 uint8_t(height >> 16), uint8_t(height >> 8), uint8_t(height), 8, 6, 0, 0, 0});
  chunk("IDAT", z);
  chunk("IEND", {});
  return std::fclose(f) == 0;
}

// --trace: forwards to the renderer and prints the order of things, draws grouped in runs of the same targets.
class TracingRenderer : public Renderer {
 public:
  TracingRenderer(vk::VulkanRenderer& renderer, const uint8_t* physical, std::string dump, uint32_t watch)
      : r_(renderer), physical_(physical), dump_(std::move(dump)), watch_(watch) {}
  ~TracingRenderer() override { EndRun(); }
  void ShaderLoaded(const Shader& shader) override { r_.ShaderLoaded(shader); }
  void Invalidate(uint32_t base, uint32_t size) override { r_.Invalidate(base, size); }
  void Flush() override { r_.Flush(); }
  void Draw(const DrawCall& draw) override {
    const uint32_t* regs = draw.registers;
    // RB_SURFACE_INFO (pitch, MSAA), RB_COLOR_INFO 0-3, RB_DEPTH_INFO, RB_MODECONTROL, RB_COLOR_MASK.
    const std::array<uint32_t, 8> key = {regs[0x2000], regs[0x2001], regs[0x2003], regs[0x2004], regs[0x2005],
                                         regs[0x2002], regs[0x2208] & 7, regs[0x2104]};
    if (!run_ || key != key_) {
      EndRun();
      run_ = true;
      key_ = key;
    }
    if (watch_) {
      // The texture fetch constants that read the watched address.
      for (uint32_t fc = 0; fc < 32; ++fc) {
        const uint32_t* d = regs + 0x4800 + 6 * fc;
        if ((d[0] & 3) == 2 && (d[1] & 0x1FFFF000) == watch_) {
          std::printf("    fetch %u reads %08X: %08X %08X %08X %08X %08X %08X (format %u)\n", fc, watch_, d[0], d[1], d[2], d[3],
                      d[4], d[5], d[1] & 0x3F);
        }
      }
    }
    const vk::RendererStats before = r_.stats();
    r_.Draw(draw);
    const vk::RendererStats& after = r_.stats();
    if (after.failed != before.failed) {
      ++failed_;
      reasons_[r_.last_error()]++;
    } else if (after.skipped != before.skipped) {
      ++skipped_;
    } else {
      ++drawn_;
    }
  }
  void Copy(const uint32_t* regs) override {
    EndRun();
    const uint32_t failed = r_.stats().failed;
    r_.Copy(regs);
    std::printf("  resolve: control %08X from surface %08X colour0 %08X depth %08X to %08X info %08X pitch %08X%s%s%s",
                regs[0x2318], regs[0x2000], regs[0x2001], regs[0x2002], regs[0x2319], regs[0x231B], regs[0x231A],
                r_.stats().failed != failed ? " FAILED: " : "", r_.stats().failed != failed ? r_.last_error().c_str() : "",
                "\n");
    // The destination as the title will read it: tiled 32-bit texels, 8-in-32 swapped (R in the low byte after).
    const uint32_t info = regs[0x231B], format = (info >> 7) & 0x3F;
    const uint32_t width = regs[0x231A] & 0x3FFF, height = (regs[0x231A] >> 16) & 0x3FFF;
    if (!dump_.empty() && format == 6 && !(regs[0x2318] & 0x8) && width && height) {
      std::vector<uint32_t> pixels(size_t(width) * height);
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          uint32_t raw;
          const uint32_t at = (regs[0x2319] & 0x1FFFFFFF) + uint32_t(vk::TiledOffset2D(int32_t(x), int32_t(y), width, 2));
          std::memcpy(&raw, physical_ + at, 4);
          pixels[size_t(y) * width + x] = ((info & 7) == 2 ? std::byteswap(raw) : raw) | 0xFF000000u;
        }
      }
      const std::string name = dump_ + "/resolve_" + std::to_string(resolves_) + ".png";
      if ((regs[0x2318] & 7) >= 4) {
        // Depth: the words as they are (depth << 8 | stencil), alpha would hide the top byte in the PNG.
        if (std::FILE* raw = std::fopen((dump_ + "/resolve_" + std::to_string(resolves_) + ".bin").c_str(), "wb")) {
          std::vector<uint32_t> words(pixels.size());
          for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
              uint32_t w;
              std::memcpy(&w, physical_ + (regs[0x2319] & 0x1FFFFFFF) + uint32_t(vk::TiledOffset2D(int32_t(x), int32_t(y), width, 2)), 4);
              words[size_t(y) * width + x] = (info & 7) == 2 ? std::byteswap(w) : w;
            }
          }
          std::fwrite(words.data(), 4, words.size(), raw);
          std::fclose(raw);
        }
      }
      ++resolves_;
      WritePng(name, width, height, pixels.data());
      std::printf("    written to %s\n", name.c_str());
    }
  }
  void Swap(uint32_t frontbuffer, uint32_t width, uint32_t height, const uint32_t* regs) override {
    EndRun();
    r_.Swap(frontbuffer, width, height, regs);
    std::printf("  swap: front buffer %08X %ux%u (fetch %08X %08X)\n", frontbuffer, width, height, regs[0x4800],
                regs[0x4801]);
  }

 private:
  void EndRun() {
    if (!run_) return;
    run_ = false;
    std::printf("  draws: surface %08X colour %08X %08X %08X %08X depth %08X mode %u mask %04X: %u drawn, %u skipped, %u failed",
                key_[0], key_[1], key_[2], key_[3], key_[4], key_[5], key_[6], key_[7], drawn_, skipped_, failed_);
    for (const auto& [why, n] : reasons_) std::printf(" [%u %s]", n, why.c_str());
    std::printf("\n");
    drawn_ = skipped_ = failed_ = 0;
    reasons_.clear();
  }
  vk::VulkanRenderer& r_;
  const uint8_t* physical_;
  std::string dump_;
  uint32_t resolves_ = 0;
  uint32_t watch_ = 0;
  bool run_ = false;
  std::array<uint32_t, 8> key_{};
  uint32_t drawn_ = 0, skipped_ = 0, failed_ = 0;
  std::map<std::string, uint32_t> reasons_;
};

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash keeps what was printed
#ifdef _WIN32
  // A crash reports where it happened (the module offset, for llvm-symbolizer).
  SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* e) -> LONG {
    const auto address = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    std::printf("gfx_replay: exception %08lX at %p (gfx_replay.exe+0x%llX)\n", e->ExceptionRecord->ExceptionCode,
                e->ExceptionRecord->ExceptionAddress, (unsigned long long)(address - base));
    return EXCEPTION_EXECUTE_HANDLER;
  });
#endif
  std::string path, out;
  bool trace = false;
  int repeat = 1;  // --repeat N: the capture replayed N times, the timings of the last (warm caches, as in a game)
  std::string dump;
  uint32_t watch = 0;
  bool bench = false;
  bool async = false;  // --async: the renderer on a thread of its own (AsyncRenderer), as in a title  // --bench: as in a title (diagnostics off, textures watched instead of hashed), kick time only
  std::string profile;  // --profile FILE: the main thread's instruction pointer sampled during the last pass
  uint32_t scale = 1;
  vk::DeviceOptions options;
  std::string pipeline_cache;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--gpu") && i + 1 < argc) options.prefer = argv[++i];
    else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else if (!std::strcmp(argv[i], "--trace")) trace = true;
    else if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = std::max(1, std::atoi(argv[++i]));
    else if (!std::strcmp(argv[i], "--dump-resolves") && i + 1 < argc) dump = argv[++i];
    else if (!std::strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
    else if (!std::strcmp(argv[i], "--bench")) bench = true;
    else if (!std::strcmp(argv[i], "--async")) async = true;
    else if (!std::strcmp(argv[i], "--pipeline-cache") && i + 1 < argc) pipeline_cache = argv[++i];
    else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) scale = uint32_t(std::clamp(std::atoi(argv[++i]), 1, 4));
    else if (!std::strcmp(argv[i], "--watch") && i + 1 < argc) watch = uint32_t(std::strtoul(argv[++i], nullptr, 16)) & 0x1FFFF000;
    else path = argv[i];
  }
  if (path.empty()) {
    std::printf("usage: gfx_replay CAPTURE [--gpu NAME] [--out DIR] [--trace] [--dump-resolves DIR] [--watch ADDR]\n");
    return 2;
  }
  vk::Device device;
  if (!device.Create(options)) {
    std::printf("gfx_replay: no Vulkan 1.3 device (%s)\n", device.error().c_str());
    return 1;
  }
  uint8_t* physical = AllocatePhysical();
  if (!physical) {
    std::printf("gfx_replay: cannot reserve %llu MB of guest memory\n", (unsigned long long)(kPhysicalBytes >> 20));
    return 1;
  }
  vk::VulkanRenderer renderer(device, physical, uint32_t(kPhysicalBytes), scale);
  renderer.SetPipelineCache(pipeline_cache);
  if (!renderer.Initialize()) {
    std::printf("gfx_replay: renderer: %s\n", renderer.last_error().c_str());
    return 1;
  }
  CommandProcessor cp(physical);
  TracingRenderer tracing(renderer, physical, dump, watch);
  Renderer* target = trace ? static_cast<Renderer*>(&tracing) : &renderer;
  std::unique_ptr<AsyncRenderer> async_renderer;
  if (async) async_renderer = std::make_unique<AsyncRenderer>(cp, *target);
  cp.SetRenderer(async_renderer ? static_cast<Renderer*>(async_renderer.get()) : target);
  uint64_t drain_ns = 0;  // waiting for the renderer's thread (part of the work with --async)
  auto drain = [&] {
    if (!async_renderer) return;
    const auto start = std::chrono::steady_clock::now();
    async_renderer->Drain();
    drain_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
  };
  cp.SetInterruptHandler([](uint32_t, uint32_t) {});

  uint32_t compared = 0, frame_index = 0;
  auto on_frame = [&](uint32_t width, uint32_t height, const uint32_t* reference) {
    drain();  // the frame is read on this thread
    const uint32_t n = frame_index++;
    std::vector<uint32_t> ours;
    if (!renderer.ReadFrame(ours) || ours.empty()) {
      std::printf("gfx_replay: frame %u: reference %ux%u, nothing presented here\n", n, width, height);
      return;
    }
    const vk::VulkanRenderer::Frame& frame = renderer.frame();
    if (scale > 1 && frame.width == width * scale && frame.height == height * scale) {
      // A scaled frame: each guest pixel's scale x scale pixels averaged, as the reference's single pixel.
      std::vector<uint32_t> down(size_t(width) * height);
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          uint32_t sum[4] = {};
          for (uint32_t sy = 0; sy < scale; ++sy) {
            for (uint32_t sx = 0; sx < scale; ++sx) {
              const uint32_t p = ours[size_t(y * scale + sy) * frame.width + x * scale + sx];
              for (int c = 0; c < 4; ++c) sum[c] += p >> (8 * c) & 0xFF;
            }
          }
          uint32_t& d = down[size_t(y) * width + x];
          d = 0;
          for (int c = 0; c < 4; ++c) d |= (sum[c] + scale * scale / 2) / (scale * scale) << (8 * c);
        }
      }
      if (!out.empty()) WritePng(out + "/frame_" + std::to_string(n) + "_scaled.png", frame.width, frame.height, ours.data());
      ours.swap(down);
    }
    if (ours.size() != size_t(width) * height) {
      std::printf("gfx_replay: frame %u: reference %ux%u, ours %ux%u\n", n, width, height, frame.width, frame.height);
    } else {
      // Pixels off by more than 8 in red, green or blue, and the mean difference per channel (a presented frame has no
      // alpha: the reference's is RGBX).
      uint64_t off = 0, sum = 0;
      for (size_t i = 0; i < ours.size(); ++i) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
          const int d = std::abs(int(ours[i] >> (8 * c) & 0xFF) - int(reference[i] >> (8 * c) & 0xFF));
          sum += d;
          worst = std::max(worst, d);
        }
        off += worst > 8;
      }
      ++compared;
      std::printf("gfx_replay: frame %u: %ux%u, %.2f%% of pixels off, mean difference %.2f per channel\n", n, width, height,
                  100.0 * double(off) / double(ours.size()), double(sum) / (3.0 * double(ours.size())));
    }
    if (!out.empty()) {
      const std::string base = out + "/frame_" + std::to_string(n);
      WritePng(base + "_ours.png", ours.size() == size_t(width) * height ? width : frame.width,
               ours.size() == size_t(width) * height ? height : frame.height, ours.data());
      WritePng(base + "_reference.png", width, height, reference);
    }
  };
  capture::ReplayStats stats;
  std::string error;
  bool ok = true;
  vk::RendererStats before;
  auto replay_start = std::chrono::steady_clock::now();
#ifdef _WIN32
  // A sampling profiler: another thread suspends this one about every millisecond and records where it was (module
  // offsets of this executable; elsewhere: 0), for llvm-symbolizer.
  std::atomic<bool> sampling{false}, sampler_done{false};
  std::vector<uint64_t> samples;
  std::map<std::string, uint32_t> outside;  // samples in other modules, by module name
  HANDLE main_thread = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
  std::thread sampler;
  if (!profile.empty()) {
    sampler = std::thread([&] {
      timeBeginPeriod(1);
      const auto base = reinterpret_cast<uint64_t>(GetModuleHandleA(nullptr));
      MODULEINFO info{};
      GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr), &info, sizeof(info));
      while (!sampler_done) {
        // About every 100 us (spinning: Sleep would wait a millisecond or more).
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
        while (std::chrono::steady_clock::now() < until) YieldProcessor();
        if (!sampling || !capture::g_replaying_kick) continue;
        if (SuspendThread(main_thread) == DWORD(-1)) continue;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(main_thread, &context)) {
          const bool own = context.Rip >= base && context.Rip < base + info.SizeOfImage;
          // Outside: the first caller in this executable (unwound), marked with bit 63.
          uint64_t caller = 0;
          if (!own) {
            CONTEXT walk = context;
            for (int depth = 0; depth < 32 && walk.Rip; ++depth) {
              if (walk.Rip >= base && walk.Rip < base + info.SizeOfImage) {
                caller = (walk.Rip - base - 1) | (1ull << 63);
                break;
              }
              DWORD64 image = 0;
              PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(walk.Rip, &image, nullptr);
              if (!entry) {
                walk.Rip = *reinterpret_cast<DWORD64*>(walk.Rsp);  // a leaf: the return address on top
                walk.Rsp += 8;
                continue;
              }
              void* handler_data = nullptr;
              DWORD64 frame = 0;
              RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, walk.Rip, entry, &walk, &handler_data, &frame, nullptr);
            }
          }
          samples.push_back(own ? context.Rip - base : caller);
          if (!own) {
            HMODULE module = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(context.Rip), &module)) {
              GetModuleBaseNameA(GetCurrentProcess(), module, name, sizeof(name));
            }
            ++outside[name];
          }
        }
        ResumeThread(main_thread);
      }
      timeEndPeriod(1);
    });
  }
#endif
  if (bench) {
    g_diagnostics = false;
    // Watched: textures are checked again only when the replay rewrites their memory (as a title's write watch).
    if (renderer.texture_cache()) renderer.texture_cache()->SetMemoryWatch([](uint32_t, uint32_t) {});
    if (std::getenv("ANY360_NO_TRACK") == nullptr) renderer.SetMemoryWatch([](uint32_t, uint32_t) {});
  }
  for (int pass = 0; pass < repeat && ok; ++pass) {
    if (pass == repeat - 1) {
      before = renderer.stats();
      replay_start = std::chrono::steady_clock::now();
    }
#ifdef _WIN32
    sampling = !profile.empty() && pass >= repeat / 2;  // the warm half
#endif
    stats = {};
    drain_ns = 0;
    // With --bench, pages the replay changes are reported as a title's write watch would (textures, vertices).
    ok = capture::Replay(path, cp, physical, kPhysicalBytes, on_frame, stats, error, drain,
                         bench ? std::function<void(uint32_t, uint32_t)>([&](uint32_t begin, uint32_t end) {
                           if (renderer.texture_cache()) renderer.texture_cache()->QueueMemoryChanged(begin, end);
                           renderer.GuestMemoryWritten(begin, end);
                         })
                               : std::function<void(uint32_t, uint32_t)>());
    drain();
    if (bench) {
      {
        static uint64_t last_gpu = 0, last_upload = 0, last_bytes = 0;
        const vk::RendererStats& g = renderer.stats();
        const double frames = std::max(1u, stats.frames);
        std::printf("gfx_replay: pass %d: GPU %.3f ms a frame running batches, %.3f copying %.2f MB of guest memory%c", pass,
                    (g.ns_gpu - last_gpu) / 1e6 / frames, (g.ns_gpu_upload - last_upload) / 1e6 / frames,
                    (g.guest_upload_bytes - last_bytes) / 1e6 / frames, 10);
        last_gpu = g.ns_gpu, last_upload = g.ns_gpu_upload, last_bytes = g.guest_upload_bytes;
      }
      if (async_renderer) {
        static uint64_t last_busy = 0, last_bytes = 0;
        const uint64_t busy = async_renderer->busy_ns(), bytes = async_renderer->queued_bytes();
        std::printf("gfx_replay: pass %d: %.2f MB queued a frame%c", pass, (bytes - last_bytes) / 1e6 / std::max(1u, stats.frames), 10);
        last_bytes = bytes;
        std::printf("gfx_replay: pass %d: %.3f ms a frame on the command processor's thread, %.3f on the renderer's, %.3f waiting for it%c",
                    pass, stats.ns_kicks / 1e6 / std::max(1u, stats.frames), (busy - last_busy) / 1e6 / std::max(1u, stats.frames),
                    drain_ns / 1e6 / std::max(1u, stats.frames), 10);
        last_busy = busy;
      }
      stats.ns_kicks += drain_ns;  // with --async: the kicks and the waits for the renderer's thread to finish them
      std::printf("gfx_replay: pass %d: %.2f ms running the kicks (%.3f ms a frame over %u frames)\n", pass, stats.ns_kicks / 1e6,
                  stats.frames ? stats.ns_kicks / 1e6 / stats.frames : 0.0, stats.frames);
    }
    renderer.Flush();
    renderer.WaitIdle();
  }
#ifdef _WIN32
  sampling = false;
  sampler_done = true;
  if (sampler.joinable()) sampler.join();
  if (!profile.empty()) {
    if (FILE* f = std::fopen(profile.c_str(), "w")) {
      for (uint64_t s : samples) std::fprintf(f, "0x%llX\n", (unsigned long long)s);
      std::fclose(f);
    }
    for (const auto& [name, count] : outside) std::printf("gfx_replay: outside: %s %u\n", name.c_str(), count);
    std::printf("gfx_replay: %zu samples written to %s\n", samples.size(), profile.c_str());
  }
#endif
  const double replay_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - replay_start).count();

  std::printf("gfx_replay: %s\n", stats.info.empty() ? path.c_str() : stats.info.c_str());
  std::printf("gfx_replay: %u kicks (%u stopped early), %u pages (%u outside), %u reference frames, %u compared, %llu presented\n",
              stats.kicks, stats.failed_kicks, stats.pages, stats.pages_outside, stats.frames, compared,
              (unsigned long long)renderer.frame().number);
  if (cp.replay_skipped_waits()) std::printf("gfx_replay: %u waits on the CPU passed\n", cp.replay_skipped_waits());
  const vk::RendererStats& r = renderer.stats();
  {
    // Where the time goes in the last pass (the frames' comparisons and PNGs included in the total).
    const double ms = 1e6;
    vk::RendererStats d = r;
    d.draws -= before.draws;
    for (auto [field, base] : {std::pair{&d.ns_draws, before.ns_draws}, {&d.ns_draw_setup, before.ns_draw_setup},
                               {&d.ns_textures, before.ns_textures}, {&d.ns_draw_targets, before.ns_draw_targets},
                               {&d.ns_draw_constants, before.ns_draw_constants}, {&d.ns_draw_record, before.ns_draw_record},
                               {&d.ns_draw_indices, before.ns_draw_indices}, {&d.ns_texture_hash, before.ns_texture_hash},
                               {&d.ns_submit, before.ns_submit}, {&d.ns_resolves, before.ns_resolves},
                               {&d.ns_swaps, before.ns_swaps}}) {
      *field -= base;
    }
    std::printf("gfx_replay: %.1f ms in all; renderer ms: draws %.1f (setup %.1f with textures %.1f and their hashing %.1f, targets %.1f, "
                "constants %.1f with indices %.1f, record %.1f), submit %.1f, resolves %.1f, swaps %.1f; %.2f us a draw\n",
                replay_ms, d.ns_draws / ms, d.ns_draw_setup / ms, d.ns_textures / ms, d.ns_texture_hash / ms,
                d.ns_draw_targets / ms,
                d.ns_draw_constants / ms, d.ns_draw_indices / ms, d.ns_draw_record / ms, d.ns_submit / ms, d.ns_resolves / ms, d.ns_swaps / ms,
                d.draws ? d.ns_draws / 1e3 / d.draws : 0.0);
  }
  std::printf("gfx_replay: %s: %u draws, %u skipped, %u failed; %u resolves, %u shaders, %u pipelines, %u render targets, "
              "%u texture uploads; %.1f ms creating pipelines\n",
              device.properties().deviceName, r.draws, r.skipped, r.failed, r.resolves, r.translations, r.pipelines,
              r.render_targets, r.texture_uploads, r.ns_pipelines / 1e6);
  // Textures over a resolve kept on the GPU that could not copy from it, and why (they read guest memory instead).
  if (renderer.texture_cache()) {
    for (const auto& [why, count] : renderer.texture_cache()->source_misses()) {
      std::printf("gfx_replay: texture over a GPU resolve, not using it: %s x%u\n", why.c_str(), count);
    }
  }
  if (!renderer.failures().empty()) {
    std::vector<std::pair<uint32_t, std::string>> reasons;
    for (const auto& [why, count] : renderer.failures()) reasons.push_back({count, why});
    std::sort(reasons.rbegin(), reasons.rend());
    std::printf("gfx_replay: not drawn, by reason:\n");
    for (const auto& [count, why] : reasons) std::printf("  %8u  %s\n", count, why.c_str());
  }
  std::vector<std::pair<uint64_t, uint32_t>> opcodes;
  for (uint32_t op = 0; op < 128; ++op) {
    if (cp.opcode_counts()[op]) opcodes.push_back({cp.opcode_counts()[op], op});
  }
  std::sort(opcodes.rbegin(), opcodes.rend());
  std::printf("gfx_replay: PM4 packets executed:\n");
  for (const auto& [count, op] : opcodes) {
    const char* name = OpcodeName(op);
    if (name) std::printf("  %8llu  %s\n", (unsigned long long)count, name);
    else std::printf("  %8llu  unknown opcode 0x%02X\n", (unsigned long long)count, op);
  }
  if (!ok) std::printf("gfx_replay: %s\n", error.c_str());
  return ok ? 0 : 1;
}
