#include "any360/gfx/capture.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "any360/gfx/command_processor.h"

namespace any360::gfx::capture {

namespace {
constexpr char kMagic[8] = {'A', '3', '6', '0', 'C', 'A', 'P', '\0'};
}

uint64_t PageHash(const uint8_t* page) {
  // Multiply-xorshift over 8-byte words: about the speed of a memory read.
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (uint32_t i = 0; i < kPageBytes; i += 8) {
    uint64_t w;
    std::memcpy(&w, page + i, 8);
    h = (h ^ w) * 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31;
  }
  return h;
}

bool Writer::Open(const std::string& path, const uint8_t* physical, uint32_t bytes, const uint32_t* registers,
                  uint32_t count, const std::string& info) {
  Close();
  file_ = std::fopen(path.c_str(), "wb");
  if (!file_) return false;
  physical_ = physical;
  bytes_ = bytes & ~(kPageBytes - 1);
  written_ = 0;
  std::fwrite(kMagic, 1, 8, file_);
  std::fwrite(&kVersion, 4, 1, file_);
  written_ += 12;
  Chunk(kInfo, info.data(), info.size());
  Chunk(kMemory, &bytes_, 4);
  Chunk(kRegisters, &count, 4, registers, uint64_t(count) * 4);
  // Every page starts as "zero": the first kick writes all the pages that hold anything.
  static const uint8_t zero[kPageBytes] = {};
  page_hashes_.assign(bytes_ / kPageBytes, PageHash(zero));
  return true;
}

void Writer::Shaders(const std::vector<uint32_t>& vertex, const std::vector<uint32_t>& pixel) {
  if (!file_) return;
  std::vector<uint32_t> payload = {uint32_t(vertex.size()), uint32_t(pixel.size())};
  payload.insert(payload.end(), vertex.begin(), vertex.end());
  payload.insert(payload.end(), pixel.begin(), pixel.end());
  Chunk(kShaders, payload.data(), payload.size() * 4);
}

void Writer::GammaRamp(const uint32_t* table, const uint32_t* pwl) {
  if (!file_) return;
  Chunk(kGammaRamp, table, 256 * 4, pwl, 384 * 4);
}

void Writer::WriteChangedPages() {
  // Hashing all of memory at every kick is the capture's cost (512 MB: tens of milliseconds): split over threads.
  const uint32_t pages = uint32_t(page_hashes_.size());
  const uint32_t workers = pages < 4096 ? 1 : std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
  std::vector<std::vector<uint32_t>> found(workers);
  auto scan = [&](uint32_t w) {
    for (uint32_t p = pages * uint64_t(w) / workers, end = pages * uint64_t(w + 1) / workers; p < end; ++p) {
      const uint64_t h = PageHash(physical_ + size_t(p) * kPageBytes);
      if (h != page_hashes_[p]) {
        page_hashes_[p] = h;
        found[w].push_back(p);
      }
    }
  };
  if (workers == 1) {
    scan(0);
  } else {
    std::vector<std::thread> threads;
    for (uint32_t w = 0; w < workers; ++w) threads.emplace_back(scan, w);
    for (std::thread& t : threads) t.join();
  }
  std::vector<uint32_t> changed;
  for (const auto& f : found) changed.insert(changed.end(), f.begin(), f.end());
  if (changed.empty()) return;
  const uint32_t count = uint32_t(changed.size());
  const uint64_t payload = 4 + uint64_t(count) * (4 + kPageBytes);
  const uint32_t header[2] = {kPages, 0};
  std::fwrite(header, 4, 2, file_);
  std::fwrite(&payload, 8, 1, file_);
  std::fwrite(&count, 4, 1, file_);
  for (uint32_t p : changed) {
    std::fwrite(&p, 4, 1, file_);
    std::fwrite(physical_ + size_t(p) * kPageBytes, 1, kPageBytes, file_);
  }
  written_ += 16 + payload;
}

void Writer::Kick(uint32_t ring_base, uint32_t ring_dwords, uint32_t read, uint32_t write) {
  if (!file_) return;
  WriteChangedPages();
  const uint32_t kick[4] = {ring_base, ring_dwords, read, write};
  Chunk(kKick, kick, sizeof(kick));
}

void Writer::Frame(uint32_t width, uint32_t height, const uint32_t* rgba) {
  if (!file_) return;
  const uint32_t size[2] = {width, height};
  Chunk(kFrame, size, 8, rgba, uint64_t(width) * height * 4);
}

void Writer::Close() {
  if (file_) std::fclose(file_);
  file_ = nullptr;
}

void Writer::Chunk(uint32_t type, const void* a, uint64_t a_bytes, const void* b, uint64_t b_bytes) {
  const uint32_t header[2] = {type, 0};
  const uint64_t payload = a_bytes + b_bytes;
  std::fwrite(header, 4, 2, file_);
  std::fwrite(&payload, 8, 1, file_);
  if (a_bytes) std::fwrite(a, 1, a_bytes, file_);
  if (b_bytes) std::fwrite(b, 1, b_bytes, file_);
  written_ += 16 + payload;
}

bool Reader::Open(const std::string& path) {
  Close();
  file_ = std::fopen(path.c_str(), "rb");
  if (!file_) return error_ = "cannot open " + path, false;
  char magic[8];
  uint32_t version = 0;
  if (std::fread(magic, 1, 8, file_) != 8 || std::memcmp(magic, kMagic, 8) || std::fread(&version, 4, 1, file_) != 1) {
    return error_ = "not an any360 capture", false;
  }
  if (version != kVersion) return error_ = "capture version " + std::to_string(version), false;
  return true;
}

bool Reader::Next(uint32_t& type, std::vector<uint8_t>& payload) {
  if (!file_) return false;
  uint32_t header[2];
  uint64_t bytes = 0;
  if (std::fread(header, 4, 2, file_) != 2) return false;  // the end
  if (std::fread(&bytes, 8, 1, file_) != 1 || bytes > (uint64_t(4) << 30)) return error_ = "damaged chunk", false;
  type = header[0];
  payload.resize(size_t(bytes));
  if (bytes && std::fread(payload.data(), 1, size_t(bytes), file_) != bytes) return error_ = "truncated chunk", false;
  return true;
}

void Reader::Close() {
  if (file_) std::fclose(file_);
  file_ = nullptr;
}

bool Replay(const std::string& path, CommandProcessor& cp, uint8_t* physical, uint64_t bytes, const FrameHandler& frame,
            ReplayStats& stats, std::string& error) {
  Reader reader;
  if (!reader.Open(path)) return error = reader.error(), false;
  uint32_t type;
  std::vector<uint8_t> payload;
  auto u32 = [&](size_t at) {
    uint32_t v = 0;
    if (at + 4 <= payload.size()) std::memcpy(&v, payload.data() + at, 4);
    return v;
  };
  while (reader.Next(type, payload)) {
    switch (type) {
      case kInfo:
        stats.info.assign(payload.begin(), payload.end());
        break;
      case kMemory:
        stats.physical_bytes = u32(0);
        break;
      case kRegisters: {
        const uint32_t count = std::min<uint32_t>(u32(0), uint32_t((payload.size() - 4) / 4));
        cp.Restore(reinterpret_cast<const uint32_t*>(payload.data() + 4), count);
        break;
      }
      case kShaders: {
        const uint32_t vs = u32(0), ps = u32(4);
        if (8 + (uint64_t(vs) + ps) * 4 > payload.size()) return error = "damaged shaders chunk", false;
        const uint32_t* words = reinterpret_cast<const uint32_t*>(payload.data() + 8);
        cp.RestoreShader(0, std::vector<uint32_t>(words, words + vs));
        cp.RestoreShader(1, std::vector<uint32_t>(words + vs, words + vs + ps));
        break;
      }
      case kPages: {
        const uint32_t count = u32(0);
        if (4 + uint64_t(count) * (4 + kPageBytes) > payload.size()) return error = "damaged pages chunk", false;
        for (uint32_t i = 0; i < count; ++i) {
          const uint8_t* entry = payload.data() + 4 + size_t(i) * (4 + kPageBytes);
          uint32_t page;
          std::memcpy(&page, entry, 4);
          if ((uint64_t(page) + 1) * kPageBytes > bytes) {
            ++stats.pages_outside;
            continue;
          }
          std::memcpy(physical + size_t(page) * kPageBytes, entry + 4, kPageBytes);
          ++stats.pages;
        }
        break;
      }
      case kGammaRamp: {
        if (payload.size() < (256 + 384) * 4) return error = "damaged gamma ramp chunk", false;
        const uint32_t* words = reinterpret_cast<const uint32_t*>(payload.data());
        cp.RestoreGammaRamp(words, words + 256);
        break;
      }
      case kKick:
        ++stats.kicks;
        {
          const auto start = std::chrono::steady_clock::now();
          g_replaying_kick = true;
          if (!cp.Replay(u32(0), u32(4), u32(8), u32(12))) ++stats.failed_kicks;
          g_replaying_kick = false;
          stats.ns_kicks += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        }
        break;
      case kFrame: {
        const uint32_t width = u32(0), height = u32(4);
        if (8 + uint64_t(width) * height * 4 > payload.size()) return error = "damaged frame chunk", false;
        ++stats.frames;
        if (frame) frame(width, height, reinterpret_cast<const uint32_t*>(payload.data() + 8));
        break;
      }
      default:
        break;  // a later version's chunk: skipped
    }
  }
  if (!reader.error().empty()) return error = reader.error(), false;
  return true;
}

}  // namespace any360::gfx::capture
