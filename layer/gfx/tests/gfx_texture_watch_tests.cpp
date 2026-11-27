// The texture cache under a host's CPU write watch (TextureCache::QueueCpuWrite, CpuWriteMark, CpuWritesVisible): a
// texture the CPU rewrites a page at a time, while draws of earlier kicks still sample it, is read and uploaded once
// per kick that follows the writes, never mid-rewrite; a write made after the watch is armed again is never lost;
// writes the GPU side reports (QueueMemoryChanged) and a host that never marks kicks keep applying at once. Ends
// with the cost of a streamed texture's frame, kick-ordered against applied at once. Skipped without a Vulkan device.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "any360/gfx/vulkan/textures.h"

namespace {

using namespace any360::gfx;

int g_failures = 0;
#define CHECK(condition)                                          \
  do {                                                            \
    if (!(condition)) {                                           \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

constexpr uint32_t kMemory = 0x100000, kBase = 0x10000, kSize = 128, kPage = 0x1000;
constexpr uint32_t kBytes = kSize * kSize * 4, kPages = kBytes / kPage;  // 64 KB: 16 pages

// A linear 128x128 k_8_8_8_8 2D texture at kBase, swizzle xyzw.
void Fetch(uint32_t (&d)[6]) {
  d[0] = 2u | (kSize / 32) << 22;
  d[1] = kBase | 6u;
  d[2] = (kSize - 1) | (kSize - 1) << 13;
  d[3] = (0u | 1u << 3 | 2u << 6 | 3u << 9) << 1;
  d[4] = 0;
  d[5] = 1u << 9;
}

struct Fixture {
  vk::Device& device;
  uint8_t* memory;
  vk::TextureCache cache;
  std::vector<std::pair<uint32_t, uint32_t>> armed;
  uint32_t fetch[6];
  Fixture(vk::Device& d, uint8_t* m) : device(d), memory(m), cache(d, m, kMemory) {
    Fetch(fetch);
    cache.SetMemoryWatch([this](uint32_t begin, uint32_t end) { armed.emplace_back(begin, end); });
  }
  bool Use() {
    std::string error;
    const VkImageView view = cache.View(fetch, vk::TextureCache::ViewType::k2DArray, error);
    if (!view) std::printf("view: %s\n", error.c_str());
    return view != VK_NULL_HANDLE;
  }
  // The CPU rewrites page `page` of the texture with `value` and the watch reports it.
  void Write(uint32_t page, uint8_t value) {
    std::memset(memory + kBase + page * kPage, value, kPage);
    cache.QueueCpuWrite(kBase + page * kPage, kBase + (page + 1) * kPage);
  }
  void Kick() { cache.CpuWritesVisible(cache.CpuWriteMark()); }
};

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  vk::DeviceOptions options;
  bool bench = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--bench")) bench = true;
    else options.prefer = argv[i];
  }
  vk::Device device;
  if (!device.Create(options)) {
    std::printf("gfx_texture_watch_tests: skipped (%s)\n", device.error().c_str());
    return 0;
  }
  auto* memory = static_cast<uint8_t*>(::operator new(kMemory, std::align_val_t(65536)));
  std::memset(memory, 0x11, kMemory);

  {
    // Kick-ordered: a rewrite in progress while draws of the previous kick sample the texture uploads nothing; the
    // kick after it uploads once, and the watch is armed again over the whole texture before it is read.
    Fixture f(device, memory);
    f.Kick();
    CHECK(f.Use() && f.cache.uploads() == 1);
    CHECK(f.armed.size() == 1 && f.armed[0].first == kBase && f.armed[0].second == kBase + kBytes);
    for (uint32_t page = 0; page < kPages; ++page) {
      f.Write(page, uint8_t(0x20 + page));
      CHECK(f.Use() && f.Use());
      CHECK(!f.cache.changes_queued());
    }
    CHECK(f.cache.uploads() == 1 && f.armed.size() == 1);
    CHECK(f.cache.cpu_writes_waiting() == kPages);
    f.Kick();
    CHECK(f.cache.changes_queued() && f.cache.cpu_writes_waiting() == 0);
    CHECK(f.Use() && f.Use() && f.Use());
    CHECK(f.cache.uploads() == 2);
    CHECK(f.armed.size() == 2 && f.armed[1].first == kBase && f.armed[1].second == kBase + kBytes);
    CHECK(!f.cache.changes_queued());

    // Written, kicked and drawn within one frame (no NewFrame): the draw after the kick sees the new content.
    f.Write(3, 0x77);
    f.Kick();
    CHECK(f.Use() && f.cache.uploads() == 3);

    // A write after the watch was armed again (here, while the last upload is in flight) is not lost: it waits for
    // the next kick and is uploaded then.
    f.Write(5, 0x78);
    CHECK(f.Use() && f.cache.uploads() == 3);
    f.Kick();
    CHECK(f.Use() && f.cache.uploads() == 4);

    // Rewritten with the same bytes: read again after the kick, not uploaded (the content hash is unchanged).
    f.Write(5, 0x78);
    f.Kick();
    CHECK(f.Use() && f.cache.uploads() == 4 && f.armed.size() == 5);

    // Writes the GPU side reports (a resolve written back, the command processor's own writes) apply at once.
    std::memset(memory + kBase, 0x99, kPage);
    f.cache.QueueMemoryChanged(kBase, kBase + kPage);
    CHECK(f.cache.changes_queued());
    CHECK(f.Use() && f.cache.uploads() == 5);

    // A write outside the texture: nothing read again.
    f.cache.QueueCpuWrite(kBase + kBytes, kBase + kBytes + kPage);
    f.Kick();
    CHECK(f.Use() && f.cache.uploads() == 5 && f.armed.size() == 6);
  }
  {
    // A host that never marks kicks: every reported write applies before the next use, as before.
    Fixture f(device, memory);
    CHECK(f.Use() && f.cache.uploads() == 1);
    f.Write(0, 0x41);
    CHECK(f.cache.changes_queued() && f.cache.cpu_writes_waiting() == 0);
    CHECK(f.Use() && f.cache.uploads() == 2);
    // Marked for the first time after writes were applied unmarked: none applied twice, the order holds from there.
    f.Write(1, 0x42);
    CHECK(f.Use() && f.cache.uploads() == 3);
    f.cache.CpuWritesVisible(0);
    f.Write(2, 0x43);
    CHECK(f.Use() && f.cache.uploads() == 3);
    f.Kick();
    CHECK(f.Use() && f.cache.uploads() == 4);
  }
  {
    // A streamed texture's frame as a title makes it: the CPU rewrites it a page at a time while the renderer, a kick
    // behind, samples it `kUses` times (draws of several passes); one kick a frame. Kick-ordered: one upload a frame;
    // applied at once: one each time a draw finds pages written since the last use.
    constexpr uint32_t kFrames = 64, kUses = 8;
    auto run = [&](bool ordered, uint32_t& uploads, double& us) {
      Fixture f(device, memory);
      if (ordered) f.Kick();
      CHECK(f.Use());
      const uint32_t first = f.cache.uploads();
      const auto start = std::chrono::steady_clock::now();
      for (uint32_t frame = 0; frame < kFrames; ++frame) {
        for (uint32_t page = 0; page < kPages; ++page) {
          f.Write(page, uint8_t(frame * 7 + page));
          if (page % (kPages / kUses) == 0) CHECK(f.Use());
        }
        if (ordered) f.Kick();
      }
      CHECK(f.Use());
      us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kFrames;
      uploads = f.cache.uploads() - first;
    };
    uint32_t ordered_uploads = 0, at_once_uploads = 0;
    double ordered_us = 0, at_once_us = 0;
    for (int pass = 0; pass < (bench ? 5 : 1); ++pass) {
      run(true, ordered_uploads, ordered_us);
      run(false, at_once_uploads, at_once_us);
    }
    CHECK(ordered_uploads == kFrames);
    CHECK(at_once_uploads >= kFrames * kUses);
    std::printf("gfx_texture_watch_tests: a %u KB texture rewritten a page at a time, used %u times a frame: kick-ordered "
                "%.2f uploads and %.1f us a frame, applied at once %.2f uploads and %.1f us a frame\n",
                kBytes >> 10, kUses, double(ordered_uploads) / kFrames, ordered_us, double(at_once_uploads) / kFrames,
                at_once_us);
  }
  ::operator delete(memory, std::align_val_t(65536));
  if (g_failures) {
    std::printf("gfx_texture_watch_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("gfx_texture_watch_tests: passed on %s\n", device.properties().deviceName);
  return 0;
}
