// Frame captures: what a GPU front end executed, recorded so any360::gfx can replay it offline and say what a real
// title's frames need (the draws it cannot make yet, the GPU units they use) and how close its image is to the
// reference's. Producer-agnostic: ReXGlue's command processor (in a title's repository) or any360::gfx's own feed
// the same writer from two hooks, a kick of the ring and a swap.
//
// File (little-endian): the magic "A360CAP\0", a u32 version, then chunks of {u32 type, u32 0, u64 payload bytes,
// payload}:
//   kInfo       UTF-8 text: the producer, the title, the place
//   kMemory     u32 physical bytes (the guest's physical memory size the capture covers)
//   kRegisters  u32 count, count u32 values: the register file when the capture starts
//   kPages      u32 count, then count times {u32 page index, 4096 bytes}: physical memory pages that changed (all
//               non-zero pages the first time)
//   kKick       u32 ring base (physical), u32 ring dwords, u32 read index, u32 write index: the ring the GPU then ran
//               from read to write (its contents are in the pages)
//   kFrame      u32 width, u32 height, width * height RGBA8 pixels (R in the low byte): the reference's presented
//               frame at a swap (optional)
//   kShaders    u32 vertex dwords, u32 pixel dwords, then the active shaders' microcode (host order) when the
//               capture starts (optional: a capture starting mid-stream otherwise lacks them until the next load)
// The pages hold the title's data: captures stay on the machine that made them, never in a repository.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace any360::gfx {
class CommandProcessor;
}

namespace any360::gfx::capture {

constexpr uint32_t kVersion = 1, kPageBytes = 4096;
enum ChunkType : uint32_t { kInfo = 1, kMemory = 2, kRegisters = 3, kPages = 4, kKick = 5, kFrame = 6, kShaders = 7 };

class Writer {
 public:
  ~Writer() { Close(); }
  // Starts a capture of `physical` (the guest's physical memory, `bytes` long) with the register file `registers`
  // (`count` dwords) as it is now.
  bool Open(const std::string& path, const uint8_t* physical, uint32_t bytes, const uint32_t* registers, uint32_t count,
            const std::string& info);
  // The active shaders when the capture starts (host-order microcode), if the producer knows them.
  void Shaders(const std::vector<uint32_t>& vertex, const std::vector<uint32_t>& pixel);
  // Before the GPU runs the ring from `read` to `write`: the pages written since the last kick, then the kick.
  void Kick(uint32_t ring_base, uint32_t ring_dwords, uint32_t read, uint32_t write);
  // The reference's frame at a swap.
  void Frame(uint32_t width, uint32_t height, const uint32_t* rgba);
  void Close();
  bool is_open() const { return file_ != nullptr; }
  uint64_t bytes_written() const { return written_; }

 private:
  void Chunk(uint32_t type, const void* a, uint64_t a_bytes, const void* b = nullptr, uint64_t b_bytes = 0);
  void WriteChangedPages();

  std::FILE* file_ = nullptr;
  const uint8_t* physical_ = nullptr;
  uint32_t bytes_ = 0;
  std::vector<uint64_t> page_hashes_;
  uint64_t written_ = 0;
};

// One chunk at a time, in file order.
class Reader {
 public:
  ~Reader() { Close(); }
  bool Open(const std::string& path);
  // The next chunk's type and payload; false at the end or on a damaged file (error()).
  bool Next(uint32_t& type, std::vector<uint8_t>& payload);
  void Close();
  const std::string& error() const { return error_; }

 private:
  std::FILE* file_ = nullptr;
  std::string error_;
};

// Replays a capture through `cp` (and the renderer it draws with) over `physical`, the guest physical memory `cp`
// executes from (`bytes` long, zero at first): the registers and shaders as the capture starts, then each change
// of memory and each kick in order; `frame` receives the reference's frames, each after the kick that presented it.
struct ReplayStats {
  uint32_t kicks = 0, failed_kicks = 0, pages = 0, pages_outside = 0, frames = 0;
  uint64_t physical_bytes = 0;  // the capture's
  std::string info;
};
using FrameHandler = std::function<void(uint32_t width, uint32_t height, const uint32_t* rgba)>;
bool Replay(const std::string& path, CommandProcessor& cp, uint8_t* physical, uint64_t bytes, const FrameHandler& frame,
            ReplayStats& stats, std::string& error);

// A fast 64-bit hash of a page (change detection, not security).
uint64_t PageHash(const uint8_t* page);

}  // namespace any360::gfx::capture
