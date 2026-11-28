// any360_bench: Any360's own benchmark title (docs/BENCH.md). Scripted, deterministic scenes run on any360::sys and
// any360::gfx the way a recompiled title runs on them, headless or in a window, with the frame report titles give.
//
//   any360_bench [--scene city] [--frames N] [--warmup N] [--objects N] [--workers N] [--window] [--gpu NAME]
//                [--any360_cpu_affinity=MASK] [--any360_gpu_device=NAME] [--sync-renderer] [--sync-pipelines] [--report-seconds S] [--json FILE] [--screenshot FILE.png]
//                [--check FILE | --write-expected FILE]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gpu.h"
#include "guest_space.h"
#include "title.h"

namespace {

using namespace any360::bench;

constexpr uint32_t kThumbWidth = 32, kThumbHeight = 18;

std::vector<uint32_t> Thumbnail(const std::vector<uint32_t>& pixels, uint32_t width, uint32_t height) {
  std::vector<uint32_t> thumb(kThumbWidth * kThumbHeight);
  for (uint32_t ty = 0; ty < kThumbHeight; ++ty) {
    for (uint32_t tx = 0; tx < kThumbWidth; ++tx) {
      uint64_t sum[3] = {}, n = 0;
      for (uint32_t y = ty * height / kThumbHeight; y < (ty + 1) * height / kThumbHeight; ++y) {
        for (uint32_t x = tx * width / kThumbWidth; x < (tx + 1) * width / kThumbWidth; ++x, ++n) {
          const uint32_t p = pixels[size_t(y) * width + x];
          for (int c = 0; c < 3; ++c) sum[c] += p >> (8 * c) & 0xFF;
        }
      }
      uint32_t v = 0;
      for (int c = 0; c < 3; ++c) v |= uint32_t(n ? sum[c] / n : 0) << (8 * c);
      thumb[ty * kThumbWidth + tx] = v;
    }
  }
  return thumb;
}

std::map<std::string, std::string> ReadExpected(const std::string& path) {
  std::map<std::string, std::string> values;
  std::ifstream in(path);
  std::string key, value;
  while (in >> key >> value) values[key] = value;
  return values;
}

std::string Hex(uint64_t v) {
  char text[32];
  std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)v);
  return text;
}

}  // namespace

int main(int argc, char** argv) {
  TitleOptions title;
  GpuOptions gpu_options;
  std::string scene = "city", json, check, write_expected, screenshot;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--scene") scene = next();
    else if (a == "--frames") title.frames = uint32_t(std::strtoul(next(), nullptr, 10));
    else if (a == "--warmup") title.warmup = uint32_t(std::strtoul(next(), nullptr, 10));
    else if (a == "--objects") title.city.objects = uint32_t(std::strtoul(next(), nullptr, 10));
    else if (a == "--workers") title.workers = uint32_t(std::strtoul(next(), nullptr, 10));
    else if (a == "--window") gpu_options.window = true;
    else if (a == "--gpu") gpu_options.gpu = next();
    else if (a.rfind("--any360_gpu_device=", 0) == 0) gpu_options.gpu = a.substr(20);
    else if (a == "--cpu-affinity" || a.rfind("--any360_cpu_affinity=", 0) == 0) {
      const std::string mask = a == "--cpu-affinity" ? std::string(next()) : a.substr(22);
      if (!KeepAffinity(mask)) {
        std::printf("any360_bench: cannot keep the processors %s\n", mask.c_str());
        return 2;
      }
    }
    else if (a == "--sync-renderer") gpu_options.async_renderer = false;
    else if (a == "--sync-pipelines") gpu_options.async_pipelines = false;
    else if (a == "--report-seconds") gpu_options.report_seconds = std::strtod(next(), nullptr);
    else if (a == "--json") json = next();
    else if (a == "--screenshot") screenshot = next();
    else if (a == "--check") check = next();
    else if (a == "--write-expected") write_expected = next();
    else if (i == 1 && a.find("--") != 0) gpu_options.gpu = a;  // the suite's GPU argument
    else {
      std::printf("any360_bench: unknown argument %s\n", a.c_str());
      return 2;
    }
  }
  if (scene != "city") {
    std::printf("any360_bench: unknown scene %s (scenes: city)\n", scene.c_str());
    return 2;
  }
  if (!check.empty() || !write_expected.empty()) gpu_options.async_pipelines = false;  // every frame drawn whole
  title.warmup = std::min(title.warmup, title.frames / 2);
  gpu_options.width = title.city.width, gpu_options.height = title.city.height;
  auto log = [](const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
  };
  gpu_options.log = log;
  title.log = log;

  GuestSpace& space = GuestSpace::Get();
  if (!space.Create()) {
    std::printf("any360_bench: no guest address space\n");
    return 1;
  }
  BenchGpu gpu;
  std::string error;
  if (!gpu.Start(gpu_options, space.physical(), [&space](uint32_t begin, uint32_t end) { space.Watch(begin, end); }, error)) {
    std::printf("any360_bench: %s\n", error.c_str());
    return 1;
  }
  space.SetWriteHook([&gpu](uint32_t begin, uint32_t end) { gpu.MemoryWritten(begin, end); });
  title.on_measure = [&gpu] { gpu.BeginMeasure(); };
  const double private_before = double(PrivateBytes()) / 1e6;
  TitleResult result;
  if (!RunTitle(title, gpu, result, error)) {
    std::printf("any360_bench: %s\n", error.c_str());
    return 1;
  }
  const RunSummary s = gpu.Summary();
  std::vector<uint32_t> pixels;
  uint32_t width = 0, height = 0;
  const bool read = gpu.ReadFrame(pixels, width, height) && width == title.city.width && height == title.city.height;
  gpu.Stop();
  if (!screenshot.empty() && read) std::printf("any360_bench: last frame %s %s\n", SavePng(screenshot, width, height, pixels) ? "written to" : "not written to", screenshot.c_str());

  const double n = std::max(1u, result.frames);
  std::printf("any360_bench: scene %s, %u frames (%u warm-up), %u objects, %u workers, %s, %s\n", scene.c_str(), result.frames,
              title.warmup, title.city.objects, title.workers, s.device.c_str(), gpu_options.window ? "windowed" : "headless");
  std::printf("any360_bench: %.1f FPS, median %.2f ms, 10%% low %.1f FPS, 1%% low %.1f FPS, slowest %.1f ms, %u over 16.7 ms\n",
              s.fps, s.median_ms, s.p10_fps, s.p1_fps, s.slowest_ms, s.over_16ms);
  std::printf("any360_bench: %s; %.2f FPS per CPU%%, %.2f FPS per GPU%%; private %.0f MB (peak %.0f, %.0f before the title)\n",
              s.usage.c_str(), s.cpu_pct > 0 ? s.fps / s.cpu_pct : 0, s.gpu_pct > 0 ? s.fps / s.gpu_pct : 0, s.private_mb,
              s.peak_private_mb, private_before);
  std::printf("any360_bench: ms a frame: title main %.3f, title render %.3f, command processor %.3f, renderer thread %.3f, "
              "draws %.3f (textures %.3f, shaders %.3f, pipelines %.3f, guest memory %.3f, submit %.3f), resolves %.3f, swaps %.3f, GPU %.3f\n",
              result.main_ms, result.render_ms, s.cp_ms, s.renderer_thread_ms, s.draws_ms, s.textures_ms, s.shaders_ms,
              s.pipelines_ms, s.memory_ms, s.submit_ms, s.resolves_ms, s.swaps_ms, s.gpu_ms);
  std::printf("any360_bench: a frame: %.0f draws, %.1f resolves, %.2f texture uploads, %.2f pipelines, %.0f system calls, %.1f watched "
              "writes, %.0f PM4 dwords, %.1f host allocations (%.0f bytes); %llu audio frames, %llu vertical blanks, %llu draws failed\n",
              s.draws, s.resolves, s.texture_uploads, s.pipelines, result.kernel_calls, double(result.write_faults) / n,
              double(result.pm4_dwords) / n, s.allocations, s.allocated_bytes, (unsigned long long)result.audio_frames, (unsigned long long)result.vblanks,
              (unsigned long long)s.total_failed);
  std::printf("any360_bench: hashes: command stream %s, audio %s\n", Hex(result.pm4_hash).c_str(), Hex(result.audio_hash).c_str());

  if (!json.empty()) {
    std::ofstream out(json);
    out << "{\n  \"scene\": \"" << scene << "\", \"device\": \"" << s.device << "\", \"frames\": " << result.frames
        << ", \"objects\": " << title.city.objects << ",\n  \"fps\": " << s.fps << ", \"median_ms\": " << s.median_ms
        << ", \"p10_fps\": " << s.p10_fps << ", \"p1_fps\": " << s.p1_fps << ", \"slowest_ms\": " << s.slowest_ms
        << ",\n  \"cpu_pct\": " << s.cpu_pct << ", \"cpu_allowed_pct\": " << s.cpu_allowed_pct << ", \"cores\": " << s.cores
        << ", \"gpu_pct\": " << s.gpu_pct << ", \"gpu_batches_pct\": " << s.gpu_own_pct << ", \"private_mb\": " << s.private_mb
        << ", \"peak_private_mb\": " << s.peak_private_mb << ",\n  \"ms\": {\"title_main\": " << result.main_ms
        << ", \"title_render\": " << result.render_ms << ", \"command_processor\": " << s.cp_ms
        << ", \"renderer_thread\": " << s.renderer_thread_ms << ", \"draws\": " << s.draws_ms << ", \"textures\": " << s.textures_ms
        << ", \"shaders\": " << s.shaders_ms << ", \"pipelines\": " << s.pipelines_ms << ", \"guest_memory\": " << s.memory_ms
        << ", \"submit\": " << s.submit_ms << ", \"resolves\": " << s.resolves_ms << ", \"swaps\": " << s.swaps_ms
        << ", \"gpu\": " << s.gpu_ms << "},\n  \"per_frame\": {\"draws\": " << s.draws << ", \"resolves\": " << s.resolves
        << ", \"texture_uploads\": " << s.texture_uploads << ", \"system_calls\": " << result.kernel_calls
        << ", \"watched_writes\": " << double(result.write_faults) / n << ", \"host_allocations\": " << s.allocations
        << ", \"host_allocated_bytes\": " << s.allocated_bytes << "},\n  \"pm4_hash\": \"" << Hex(result.pm4_hash)
        << "\", \"audio_hash\": \"" << Hex(result.audio_hash) << "\"\n}\n";
  }

  const std::vector<uint32_t> thumb = read ? Thumbnail(pixels, width, height) : std::vector<uint32_t>{};
  if (!write_expected.empty()) {
    std::ofstream out(write_expected);
    out << "frames " << title.frames << "\nobjects " << title.city.objects << "\npm4_hash " << Hex(result.pm4_hash)
        << "\naudio_hash " << Hex(result.audio_hash) << "\nthumbnail ";
    for (uint32_t v : thumb) {
      char text[8];
      std::snprintf(text, sizeof(text), "%06x", v & 0xFFFFFF);
      out << text;
    }
    out << "\n";
    std::printf("any360_bench: expected values written to %s\n", write_expected.c_str());
  }
  if (check.empty()) return s.total_failed ? 1 : 0;

  int failures = 0;
  auto expect = [&](bool ok, const std::string& what) {
    if (!ok) ++failures;
    std::printf("any360_bench: check %s: %s\n", what.c_str(), ok ? "ok" : "FAILED");
  };
  const auto expected = ReadExpected(check);
  auto value = [&](const char* key) {
    const auto it = expected.find(key);
    return it == expected.end() ? std::string() : it->second;
  };
  expect(value("frames") == std::to_string(title.frames) && value("objects") == std::to_string(title.city.objects),
         "the expected file's run (" + value("frames") + " frames, " + value("objects") + " objects) is this run's");
  expect(result.frames == title.frames, "every frame swapped (" + std::to_string(result.frames) + ")");
  expect(s.total_failed == 0, "no draw failed");
  expect(value("pm4_hash") == Hex(result.pm4_hash), "the command stream is the expected one (deterministic title)");
  expect(result.audio_frames >= kAudioHashFrames && value("audio_hash") == Hex(result.audio_hash), "the audio is the expected one");
  expect(result.write_faults > 0, "the CPU's texture writes reached the renderer through the write watch");
  uint32_t worst = 0;
  const std::string text = value("thumbnail");
  if (read && text.size() == 6 * thumb.size()) {
    for (size_t i = 0; i < thumb.size(); ++i) {
      const uint32_t e = uint32_t(std::strtoul(text.substr(6 * i, 6).c_str(), nullptr, 16));
      for (int c = 0; c < 3; ++c) worst = std::max<uint32_t>(worst, uint32_t(std::abs(int(e >> (8 * c) & 0xFF) - int(thumb[i] >> (8 * c) & 0xFF))));
    }
    expect(worst <= 12, "the last frame matches the expected picture (largest difference " + std::to_string(worst) + " of 255)");
  } else {
    expect(false, "the last frame was read back");
  }
  expect(s.peak_private_mb - private_before < 1024, "the process's memory stays bounded");
  std::printf("any360_bench: %d failing checks\n", failures);
  return failures ? 1 : 0;
}
