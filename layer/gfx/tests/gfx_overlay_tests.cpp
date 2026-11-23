// The frame counter Any360 draws over every title's picture (--any360_fps_overlay; gfx/frame_counter.h and
// vk::Presenter): its frame time statistics against the binding's first version of them, its text, its bitmap
// against hand-drawn glyphs and its own scaled-up and clipped forms, the screenshot composite, and the presenter's
// copy onto a swapchain-like image on the GPU (read back pixel for pixel; under the validation layer in the suite).
//
//   gfx_overlay_tests [GPU]            the tests (GPU: a substring of the device name)
//   gfx_overlay_tests [GPU] --bench    and what the counter costs a frame, on and off: CPU time, bytes and allocations,
//                                      the GPU's time for the copy
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include "any360/gfx/frame_counter.h"
#include "any360/gfx/vulkan/device.h"
#include "any360/gfx/vulkan/presenter.h"

using namespace any360::gfx;

namespace {
int g_checks = 0, g_failures = 0;
#define CHECK(condition)                                                                                  \
  do {                                                                                                    \
    ++g_checks;                                                                                           \
    if (!(condition) && ++g_failures <= 20) std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  } while (0)

// Every allocation of the process, counted (bytes and calls) while g_counting is set.
std::atomic<bool> g_counting{false};
std::atomic<uint64_t> g_allocs{0}, g_bytes{0};
}  // namespace

void* operator new(size_t size) {
  if (g_counting.load(std::memory_order_relaxed)) g_allocs.fetch_add(1), g_bytes.fetch_add(size);
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {

// The binding's first version of the report's statistics (rexglue.cpp TakeFrameTimes before it moved here).
FrameTimeSummary Reference(std::vector<float> v) {
  FrameTimeSummary s;
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  const auto at = [&](double q) { return double(v[size_t(q * double(v.size() - 1) + 0.5)]); };
  s.frames = uint32_t(v.size()), s.median_ms = at(0.5), s.p90_ms = at(0.9), s.p99_ms = at(0.99);
  return s;
}
bool Same(const FrameTimeSummary& a, const FrameTimeSummary& b) {
  return a.frames == b.frames && a.median_ms == b.median_ms && a.p90_ms == b.p90_ms && a.p99_ms == b.p99_ms;
}

void TestStatistics() {
  std::mt19937 rng(360);
  for (int trial = 0; trial < 200; ++trial) {
    std::vector<float> v(1 + rng() % 2000);
    for (float& x : v) x = 4.0f + float(rng() % 40000) / 1000.0f;
    std::vector<float> copy = v;
    CHECK(Same(SummarizeFrameTimes(copy), Reference(v)));
  }
  CHECK(SummarizeFrameTimes({}).frames == 0);
  std::vector<float> one = {16.0f};
  const FrameTimeSummary s1 = SummarizeFrameTimes(one);
  CHECK(s1.frames == 1 && s1.median_ms == 16.0 && s1.p90_ms == 16.0 && s1.p99_ms == 16.0);
  // 100 frames: 98 of 10 ms and two of 50: the 1% low is 20 FPS, the 10% low and the median 100.
  std::vector<float> hitch(100, 10.0f);
  hitch[17] = hitch[60] = 50.0f;
  const FrameTimeSummary sh = SummarizeFrameTimes(hitch);
  CHECK(sh.median_ms == 10.0 && sh.p90_ms == 10.0 && sh.p99_ms == 50.0);

  // The window keeps the last kWindow times, without allocating.
  FrameTimeWindow window;
  CHECK(window.count() == 0 && window.Summary().frames == 0);
  std::vector<float> all;
  g_allocs = 0, g_counting = true;
  for (uint32_t i = 0; i < 3 * FrameTimeWindow::kWindow + 77; ++i) {
    const float ms = 5.0f + float((i * 7919) % 300) / 10.0f;
    window.Add(ms);
    if (i % 97 == 0) window.Summary();
  }
  g_counting = false;
  CHECK(g_allocs == 0);
  for (uint32_t i = 0; i < 3 * FrameTimeWindow::kWindow + 77; ++i) all.push_back(5.0f + float((i * 7919) % 300) / 10.0f);
  CHECK(window.count() == FrameTimeWindow::kWindow);
  CHECK(Same(window.Summary(), Reference(std::vector<float>(all.end() - FrameTimeWindow::kWindow, all.end()))));
  CHECK(Same(window.Summary(), window.Summary()));  // the ring is not disturbed by a summary
  window.Clear();
  window.Add(8.0f);
  CHECK(window.count() == 1 && window.Summary().median_ms == 8.0);
}

void TestText() {
  OverlayFigures f;
  f.fps = 59.6, f.frame_ms = 16.78, f.max_ms = 33.4;
  f.window = {512, 16.7, 18.2, 40.0};
  f.gpu_ms = 3.14, f.cp_idle_pct = 41.6;
  CHECK(FormatOverlay(f) == "60 FPS 16.8 ms (max 33.4)\nmedian 16.7 ms, 10% low 55, 1% low 25 FPS\nGPU 3.1 ms CP idle 42%");
  f.gpu_ms = -1;
  CHECK(FormatOverlay(f) == "60 FPS 16.8 ms (max 33.4)\nmedian 16.7 ms, 10% low 55, 1% low 25 FPS\nCP idle 42%");
  f.cp_idle_pct = -1;
  CHECK(FormatOverlay(f) == "60 FPS 16.8 ms (max 33.4)\nmedian 16.7 ms, 10% low 55, 1% low 25 FPS");
  f.window = {};
  CHECK(FormatOverlay(f) == "60 FPS 16.8 ms (max 33.4)");
  // Every character the text can hold has a glyph (none drawn blank).
  std::mt19937 rng(7);
  for (int trial = 0; trial < 500; ++trial) {
    OverlayFigures r;
    r.fps = double(rng() % 5000), r.frame_ms = double(rng() % 100000) / 100.0, r.max_ms = double(rng() % 100000) / 10.0;
    r.window = {1 + uint32_t(rng() % 512), 0.1 + double(rng() % 10000) / 10.0, 0.1 + double(rng() % 10000) / 10.0,
                0.1 + double(rng() % 10000) / 10.0};
    r.gpu_ms = double(rng() % 10000) / 100.0, r.cp_idle_pct = double(rng() % 101);
    const std::string text = FormatOverlay(r);
    CHECK(std::all_of(text.begin(), text.end(), OverlayHasGlyph));
  }
  for (char c : std::string("0123456789.,:()/%-+ CFGPRSUadeilmnosx")) CHECK(OverlayHasGlyph(c));
  CHECK(!OverlayHasGlyph('Q') && !OverlayHasGlyph('z'));
}

std::vector<uint32_t> Raster(std::string_view text, uint32_t scale, uint32_t& w, uint32_t& h) {
  const OverlaySize size = MeasureOverlay(text, scale);
  w = size.width, h = size.height;
  std::vector<uint32_t> px(size_t(w) * h, 0xDEADBEEF);
  RasterizeOverlay(text, scale, 1, 0, px.data(), w, h);
  return px;
}

void TestBitmap() {
  CHECK(MeasureOverlay("", 2).width == 0 && MeasureOverlay("", 2).height == 0);
  CHECK(MeasureOverlay("12", 1).width == 14 && MeasureOverlay("12", 1).height == 9);
  CHECK(MeasureOverlay("1\n123", 3).width == 60 && MeasureOverlay("1\n123", 3).height == 51);
  CHECK(OverlayScale(200) == 1 && OverlayScale(720) == 1 && OverlayScale(1080) == 2 && OverlayScale(1440) == 2 &&
        OverlayScale(2160) == 4 && OverlayScale(4320) == 4);
  // '1', drawn by hand: a cell of 6 x 8 font pixels after a pixel of border.
  static const char* kOne[9] = {"        ", " ..#... ", " .##... ", " ..#... ", " ..#... ",
                                " ..#... ", " ..#... ", " .###.. ", "        "};
  uint32_t w = 0, h = 0;
  const std::vector<uint32_t> one = Raster("1", 1, w, h);
  CHECK(w == 8 && h == 9);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) CHECK(one[size_t(y) * w + x] == (kOne[y][x] == '#' ? 1u : 0u));
  }
  // Two lines: the second line's glyphs 8 font pixels lower; a blank (unknown) character draws nothing.
  const std::vector<uint32_t> two = Raster("1\nQ1", 1, w, h);
  CHECK(w == 14 && h == 17);
  for (uint32_t y = 0; y < 9; ++y) {
    for (uint32_t x = 0; x < 8; ++x) {
      CHECK(two[size_t(y) * w + x] == one[size_t(y) * 8 + x]);
      CHECK(two[size_t(y + 8) * w + x + 6] == one[size_t(y) * 8 + x]);  // "1" after the blank Q, a line down
    }
  }
  for (uint32_t y = 9; y < 17; ++y) {
    for (uint32_t x = 0; x < 7; ++x) CHECK(two[size_t(y) * w + x] == 0);  // the Q's cell
  }
  // Each scale is the first one blown up; a clipped band is the full one cropped.
  const std::string text = "60 FPS 16.8 ms (max 33.4)\nmedian 16.7 ms, 10% low 55, 1% low 25 FPS\nGPU 3.1 ms CP idle 42%";
  uint32_t w1 = 0, h1 = 0;
  const std::vector<uint32_t> base = Raster(text, 1, w1, h1);
  for (uint32_t scale = 2; scale <= 4; ++scale) {
    const std::vector<uint32_t> big = Raster(text, scale, w, h);
    CHECK(w == w1 * scale && h == h1 * scale);
    bool same = true;
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) same = same && big[size_t(y) * w + x] == base[size_t(y / scale) * w1 + x / scale];
    }
    CHECK(same);
    const uint32_t cw = w / 3, ch = h / 2;
    std::vector<uint32_t> clipped(size_t(cw) * ch);
    RasterizeOverlay(text, scale, 1, 0, clipped.data(), cw, ch);
    bool cropped = true;
    for (uint32_t y = 0; y < ch; ++y) {
      for (uint32_t x = 0; x < cw; ++x) cropped = cropped && clipped[size_t(y) * cw + x] == big[size_t(y) * w + x];
    }
    CHECK(cropped);
  }
  CHECK(OverlayInk(false) == 0xFF40FFFFu && OverlayInk(true) == 0xFFFFFF40u);  // yellow in RGBA8 and in BGRA8

  // The screenshot composite: the band over the top-left corner in RGBA8 at the image's scale, the rest untouched.
  const uint32_t iw = 1920, ih = 1080;
  std::vector<uint32_t> image(size_t(iw) * ih, 0x12345678u);
  CompositeOverlayRgba(text, image.data(), iw, ih);
  const std::vector<uint32_t> band = Raster(text, 2, w, h);
  bool composite = true;
  for (uint32_t y = 0; y < ih; ++y) {
    for (uint32_t x = 0; x < iw; ++x) {
      const uint32_t want = x < w && y < h ? (band[size_t(y) * w + x] ? OverlayInk(false) : kOverlayPaper) : 0x12345678u;
      composite = composite && image[size_t(y) * iw + x] == want;
    }
  }
  CHECK(composite);
  // An image narrower than the band: clipped, nothing written past its rows.
  std::vector<uint32_t> narrow(size_t(100) * 400 + 1, 7);
  CompositeOverlayRgba(text, narrow.data(), 100, 400);
  CHECK(narrow.back() == 7 && narrow[size_t(399) * 100 + 99] == 7 && narrow[0] == kOverlayPaper);
}

void Barrier(const vk::Functions& f, VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = from, b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  d.imageMemoryBarrierCount = 1, d.pImageMemoryBarriers = &b;
  f.vkCmdPipelineBarrier2(cb, &d);
}

// The presenter's per-frame draw (RecordOverlayCopy) onto a BGRA8 image the size of a swapchain's, read back: the band
// in its top-left corner, the rest as cleared. With `bench`: the GPU's time for one copy, from timestamps.
void TestGpu(vk::Device& device, const std::string& text, uint32_t iw, uint32_t ih, bool bench) {
  const vk::Functions& f = device.fn();
  const uint32_t scale = OverlayScale(ih);
  const OverlaySize size = MeasureOverlay(text, scale);
  const uint32_t w = std::min(size.width, iw), h = std::min(size.height, ih);
  vk::Image image;
  vk::Buffer band, readback;
  CHECK(vk::CreateImage2D(device, iw, ih, VK_FORMAT_B8G8R8A8_UNORM,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                          image));
  CHECK(vk::CreateBuffer(device, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, band));
  CHECK(vk::CreateBuffer(device, VkDeviceSize(iw) * ih * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readback));
  if (!image.image || !band.mapped || !readback.mapped) return;
  RasterizeOverlay(text, scale, OverlayInk(true), kOverlayPaper, static_cast<uint32_t*>(band.mapped), w, h);
  constexpr uint32_t kCopies = 240;
  VkQueryPool queries = VK_NULL_HANDLE;
  VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qi.queryType = VK_QUERY_TYPE_TIMESTAMP, qi.queryCount = 4;
  CHECK(f.vkCreateQueryPool(device.device(), &qi, nullptr, &queries) == VK_SUCCESS);
  CHECK(device.Submit([&](VkCommandBuffer cb) {
    f.vkCmdResetQueryPool(cb, queries, 0, 4);
    Barrier(f, cb, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue grey{{0.5f, 0.25f, 0.75f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdClearColorImage(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &grey, 1, &range);
    // As many frames' copies as a few seconds of play, timed; the off case records nothing (Presenter::DrawOverlay
    // returns before recording when the text is empty), timed the same way for the comparison.
    f.vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 0);
    for (uint32_t i = 0; i < (bench ? kCopies : 1); ++i) vk::RecordOverlayCopy(f, cb, band.buffer, image.image, w, h);
    f.vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 1);
    f.vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 2);
    f.vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 3);
    Barrier(f, cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {iw, ih, 1};
    f.vkCmdCopyImageToBuffer(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
  }));
  const uint32_t* px = static_cast<const uint32_t*>(readback.mapped);
  std::vector<uint32_t> want(size_t(w) * h);
  RasterizeOverlay(text, scale, OverlayInk(true), kOverlayPaper, want.data(), w, h);
  const uint32_t cleared = 0xFF000000u | (128u << 16) | (64u << 8) | 191u;  // B8G8R8A8 of the clear: R .5, G .25, B .75
  uint32_t wrong = 0;
  for (uint32_t y = 0; y < ih; ++y) {
    for (uint32_t x = 0; x < iw; ++x) {
      const uint32_t expect = x < w && y < h ? want[size_t(y) * w + x] : cleared;
      const uint32_t got = px[size_t(y) * iw + x];
      // The clear's rounding may differ by one step a channel between drivers.
      const bool close = x < w && y < h ? got == expect
                                        : std::abs(int(got & 0xFF) - int(expect & 0xFF)) <= 1 &&
                                              std::abs(int(got >> 8 & 0xFF) - int(expect >> 8 & 0xFF)) <= 1 &&
                                              std::abs(int(got >> 16 & 0xFF) - int(expect >> 16 & 0xFF)) <= 1;
      wrong += !close;
    }
  }
  CHECK(wrong == 0);
  if (bench) {
    uint64_t t[4] = {};
    CHECK(f.vkGetQueryPoolResults(device.device(), queries, 0, 4, sizeof(t), t, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS);
    const double period = device.properties().limits.timestampPeriod;
    std::printf("  GPU, %ux%u window (band %ux%u, %.0f KB): on %.2f us a frame (%u copies), off %.2f us (nothing recorded)\n",
                iw, ih, w, h, double(w) * h * 4 / 1024.0, double(t[1] - t[0]) * period / 1e3 / kCopies, kCopies,
                double(t[3] - t[2]) * period / 1e3);
  }
  f.vkDestroyQueryPool(device.device(), queries, nullptr);
  vk::DestroyBuffer(device, band);
  vk::DestroyBuffer(device, readback);
  vk::DestroyImage(device, image);
}

double PrivateMb() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX c{};
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof(c))) {
    return double(c.PrivateUsage) / 1048576.0;
  }
#endif
  return 0;
}

// The binding's per-frame work with the counter on (PresentingRenderer::Overlay and the presenter's DrawOverlay, CPU
// side) at `fps` frames a second: each frame its time into the window; twice a second the summary, the text, and the
// band rasterized for each of the presenter's three frame slots. Off: Overlay is not called and DrawOverlay returns at
// once (an empty text): nothing to time but the branch.
void BenchCpu(double fps, uint32_t height) {
  constexpr uint32_t kFrames = 2'000'000;
  const uint32_t refresh = std::max(1u, uint32_t(fps / 2));
  const double mb0 = PrivateMb();
  FrameTimeWindow window;
  std::string shown;
  std::vector<uint32_t> slots[3];
  const uint32_t scale = OverlayScale(height);
  for (auto& s : slots) s.resize(size_t(MeasureOverlay(std::string(48, '0') + "\n\n", scale).width) * (25 * scale));
  volatile bool on = true;
  for (int variant = 0; variant < 2; ++variant) {
    on = variant == 0;
    std::mt19937 rng(1);
    g_allocs = 0, g_bytes = 0, g_counting = true;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kFrames; ++i) {
      if (!on) continue;
      window.Add(float(1e3 / fps) + float(rng() % 1000) / 1000.0f);
      if (i % refresh) continue;
      OverlayFigures figures;
      figures.fps = fps, figures.frame_ms = 1e3 / fps, figures.max_ms = 2e3 / fps;
      figures.window = window.Summary();
      figures.gpu_ms = 3.0, figures.cp_idle_pct = 40.0;
      std::string text = FormatOverlay(figures);
      if (text == shown) continue;
      shown = std::move(text);
      const OverlaySize size = MeasureOverlay(shown, scale);
      for (auto& s : slots) {
        if (s.size() < size_t(size.width) * size.height) s.resize(size_t(size.width) * size.height);
        RasterizeOverlay(shown, scale, OverlayInk(true), kOverlayPaper, s.data(), size.width, size.height);
      }
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kFrames;
    g_counting = false;
    std::printf("  CPU at %.0f FPS, %up: %-3s %8.2f ns a frame, %6.3f allocations and %8.1f bytes a frame\n", fps,
                height, on ? "on" : "off", ns, double(g_allocs) / kFrames, double(g_bytes) / kFrames);
  }
  std::printf("  private memory: %.2f MB more with the counter's buffers allocated (window %zu bytes, 3 bands %zu bytes)\n",
              PrivateMb() - mb0, sizeof(FrameTimeWindow), 3 * slots[0].capacity() * 4);
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu;
  bool bench = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--bench")) {
      bench = true;
    } else {
      gpu = argv[i];
    }
  }
  TestStatistics();
  TestText();
  TestBitmap();
  const std::string text = "60 FPS 16.8 ms (max 33.4)\nmedian 16.7 ms, 10% low 55, 1% low 25 FPS\nGPU 3.1 ms CP idle 42%";
  if (bench) {
    std::printf("gfx_overlay_tests: the frame counter's cost a frame\n");
    BenchCpu(60, 1080);
    BenchCpu(240, 1440);
  }
  {
    vk::Device device;
    vk::DeviceOptions options;
    options.prefer = gpu;
    if (device.Create(options)) {
      std::printf("gfx_overlay_tests: %s\n", device.properties().deviceName);
      TestGpu(device, text, 1920, 1080, bench);
      TestGpu(device, text, 2560, 1440, bench);
      TestGpu(device, text, 300, 200, false);  // a window smaller than the band: clipped
    } else {
      std::printf("gfx_overlay_tests: GPU part skipped (%s)\n", device.error().c_str());
    }
  }
  std::printf("gfx_overlay_tests: %d checks, %d failing checks\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
