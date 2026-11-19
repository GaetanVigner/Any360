// LDI (xboxkrnl's LZX decompression interface) and Any360's LZX decoder. The decoder is checked against the host's
// LZX encoder: on Windows, cabinet.dll's FCI compresses generated data (text, random bytes, x86-like code with E8
// calls, long-distance repeats, sizes around a frame) into CAB files of every window size; each CFDATA block of the
// CAB is one LDI chunk (one 32 KiB frame), decoded in order and compared byte for byte with the data. Everywhere,
// hand-built streams of uncompressed blocks (an odd length, a block across chunks) and corrupt input. The contexts:
// the title's allocator and freer called as the kernel's (sizes, order, the failure path), the handle's magic, the
// parameter and size results Xenia gives, no host allocation once warm.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "any360/sys/alloc_meter.h"
#include "any360/sys/ldi.h"

#include "lzx_cab.h"

namespace {

using namespace any360::sys;

int g_failures = 0;

#define CHECK(condition)                                                          \
  do {                                                                            \
    if (!(condition)) {                                                           \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition);                 \
      ++g_failures;                                                               \
    }                                                                             \
  } while (0)

using Bytes = std::vector<uint8_t>;

// --- generated data ---

Bytes Text(size_t size, uint32_t seed) {
  static const char* kWords[] = {"the ", "kernel ", "frame ", "window ", "of ", "LZX ", "decoder ", "block ", "\n",
                                 "tree ", "match ", "offset ", "length ", "and ", "a ", "title ", "chunk ", ", "};
  std::mt19937 rng(seed);
  Bytes out;
  while (out.size() < size) {
    const char* w = kWords[rng() % std::size(kWords)];
    out.insert(out.end(), w, w + std::strlen(w));
  }
  out.resize(size);
  return out;
}

Bytes Random(size_t size, uint32_t seed) {
  std::mt19937 rng(seed);
  Bytes out(size);
  for (auto& b : out) b = uint8_t(rng());
  return out;
}

// x86-like code: short instruction-ish runs and CALLs (E8 + a 32-bit target), so the E8 translation is used.
Bytes Code(size_t size, uint32_t seed) {
  std::mt19937 rng(seed);
  Bytes out;
  static const uint8_t kOps[][3] = {{0x8B, 0x45, 0xF8}, {0x89, 0x4D, 0xFC}, {0x83, 0xC4, 0x08}, {0x55, 0x8B, 0xEC},
                                    {0x5D, 0xC3, 0x90}, {0x33, 0xC0, 0x40}};
  while (out.size() < size) {
    if (rng() % 4 == 0) {
      out.push_back(0xE8);
      const uint32_t target = rng() % 0x60000;
      for (int i = 0; i < 4; ++i) out.push_back(uint8_t(target >> (8 * i)));
    } else {
      const auto& op = kOps[rng() % std::size(kOps)];
      out.insert(out.end(), op, op + 3);
    }
  }
  out.resize(size);
  return out;
}

// A random block repeated with small edits: matches far back (past small windows).
Bytes Repeats(size_t size, size_t period, uint32_t seed) {
  std::mt19937 rng(seed);
  const Bytes block = Random(period, seed + 1);
  Bytes out;
  while (out.size() < size) {
    out.insert(out.end(), block.begin(), block.end());
    for (int i = 0; i < 16; ++i) out[out.size() - 1 - rng() % period] = uint8_t(rng());
  }
  out.resize(size);
  return out;
}

Bytes Mixed(size_t size, uint32_t seed) {
  Bytes out;
  uint32_t part = 0;
  while (out.size() < size) {
    const size_t n = 20000 + (seed * 7919 + part * 104729) % 90000;
    const Bytes piece = part % 4 == 0 ? Text(n, seed + part)
                        : part % 4 == 1 ? Code(n, seed + part)
                        : part % 4 == 2 ? Repeats(n, 3000, seed + part)
                                        : Random(n / 8, seed + part);
    out.insert(out.end(), piece.begin(), piece.end());
    ++part;
  }
  out.resize(size);
  return out;
}

// --- decoding a stream of chunks ---

struct Chunk {
  Bytes data;
  uint32_t frame;
};

bool DecodeAll(LzxDecoder& decoder, const std::vector<Chunk>& chunks, Bytes& out) {
  out.clear();
  Bytes frame(LzxDecoder::kFrameSize);
  for (const Chunk& c : chunks) {
    if (!decoder.Decompress(c.data.data(), c.data.size(), frame.data(), c.frame)) return false;
    out.insert(out.end(), frame.begin(), frame.begin() + c.frame);
  }
  return true;
}

// --- hand-built streams (every platform) ---

// LZX's bit order: 16-bit little-endian words, most significant bit first.
struct BitWriter {
  Bytes out;
  uint32_t word = 0;
  int bits = 0;
  void Put(uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) {
      word = word << 1 | ((value >> i) & 1);
      if (++bits == 16) Flush();
    }
  }
  void Flush() {
    out.push_back(uint8_t(word));
    out.push_back(uint8_t(word >> 8));
    word = 0;
    bits = 0;
  }
  // An uncompressed block's header alignment: to the next word, a whole word when on one.
  void AlignForBytes() {
    if (!bits) Put(0, 16);
    else Put(0, 16 - bits);
  }
  void Raw(const uint8_t* data, size_t size) { out.insert(out.end(), data, data + size); }
};

void UncompressedHeader(BitWriter& w, uint32_t length, uint32_t r0 = 1, uint32_t r1 = 1, uint32_t r2 = 1) {
  w.Put(3, 3);
  w.Put(length >> 8, 16);
  w.Put(length & 0xFF, 8);
  w.AlignForBytes();
  for (const uint32_t r : {r0, r1, r2}) {
    for (int i = 0; i < 4; ++i) w.out.push_back(uint8_t(r >> (8 * i)));
  }
}

void HandBuiltTests() {
  // Two uncompressed blocks: 5 bytes (odd: one byte of padding follows), then 40000 bytes across two chunks (two
  // frames: 32768 and 7237).
  const Bytes data = Random(40005, 11);
  BitWriter w;
  w.Put(0, 1);  // no E8 translation
  UncompressedHeader(w, 5);
  w.Raw(data.data(), 5);
  w.out.push_back(0);  // the padding of an odd block
  UncompressedHeader(w, 40000);
  const size_t first_raw = 32768 - 5;
  w.Raw(data.data() + 5, first_raw);
  Chunk one{w.out, 32768};
  Chunk two{Bytes(data.begin() + 5 + long(first_raw), data.end()), uint32_t(40005 - 32768)};
  for (uint32_t bits = 15; bits <= 21; ++bits) {
    LzxDecoder decoder;
    CHECK(decoder.Init(bits));
    Bytes out;
    CHECK(DecodeAll(decoder, {one, two}, out) && out == data);
    // Again after a reset: the same output.
    decoder.Reset();
    CHECK(DecodeAll(decoder, {one, two}, out) && out == data);
  }
  // An odd block that ends with its frame: its padding byte in that chunk (as cabinet.dll writes it) or at the next
  // chunk's start, and the next block's header after it.
  {
    const Bytes more = Random(32768 + 300, 12);
    for (const bool pad_in_first : {true, false}) {
      BitWriter a;
      a.Put(0, 1);
      UncompressedHeader(a, 1);
      a.Raw(more.data(), 1);
      a.out.push_back(0);
      UncompressedHeader(a, 32767);
      a.Raw(more.data() + 1, 32767);
      Bytes second;
      (pad_in_first ? a.out : second).push_back(0);
      BitWriter b;
      UncompressedHeader(b, 300);
      b.Raw(more.data() + 32768, 300);
      second.insert(second.end(), b.out.begin(), b.out.end());
      LzxDecoder decoder;
      CHECK(decoder.Init(16));
      Bytes out;
      CHECK(DecodeAll(decoder, {{a.out, 32768}, {second, 300}}, out) && out == more);
    }
  }
  // Window sizes outside 15-21 bits, a frame of 0 or past 32 KiB, a decoder never set up.
  LzxDecoder decoder;
  uint8_t frame[LzxDecoder::kFrameSize + 1];
  CHECK(!decoder.Init(14) && !decoder.Init(22));
  CHECK(!decoder.Decompress(one.data.data(), one.data.size(), frame, 32768));
  CHECK(decoder.Init(15));
  CHECK(!decoder.Decompress(one.data.data(), one.data.size(), frame, 0));
  CHECK(!decoder.Decompress(one.data.data(), one.data.size(), frame, LzxDecoder::kFrameSize + 1));
  // A chunk that ends early, a bad block type.
  decoder.Reset();
  CHECK(!decoder.Decompress(one.data.data(), 100, frame, 32768));
  BitWriter bad;
  bad.Put(0, 1);
  bad.Put(5, 3);
  bad.Put(0, 24);
  bad.Flush();
  decoder.Reset();
  CHECK(!decoder.Decompress(bad.out.data(), bad.out.size(), frame, 100));
  // Random chunks: refused or decoded, never past the buffers (checked by the debug runtime and ASan builds).
  std::mt19937 rng(5);
  int refused = 0;
  for (int i = 0; i < 3000; ++i) {
    const Bytes junk = Random(1 + rng() % 4000, uint32_t(i));
    decoder.Init(15 + i % 7);
    refused += !decoder.Decompress(junk.data(), junk.size(), frame, 1 + rng() % LzxDecoder::kFrameSize);
  }
  CHECK(refused > 2900);
}

#if defined(_WIN32)

// The host's LZX encoder (lzx_cab.h): FCI's chunks of `data` with a window of 2^bits.
std::vector<Chunk> CabChunks(const Bytes& data, uint32_t bits) {
  std::vector<Chunk> chunks;
  for (auto& c : any360::test::cab::CabChunks(data, bits)) chunks.push_back({std::move(c.data), c.frame});
  return chunks;
}

void CabTests() {
  struct Case {
    const char* name;
    Bytes data;
  };
  std::vector<Case> cases;
  cases.push_back({"one byte", Text(1, 1)});
  cases.push_back({"ten bytes", Code(10, 2)});
  cases.push_back({"one frame", Text(32768, 3)});
  cases.push_back({"one frame and a byte", Text(32769, 4)});
  cases.push_back({"text", Text(300000, 5)});
  cases.push_back({"random", Random(140000, 6)});
  cases.push_back({"code", Code(400000, 7)});
  cases.push_back({"far repeats", Repeats(600000, 70000, 8)});
  cases.push_back({"mixed", Mixed(2500000, 9)});
  int checked = 0, translated = 0;
  for (const Case& c : cases) {
    for (uint32_t bits = 15; bits <= 21; ++bits) {
      if (c.data.size() > 1000000 && bits != 15 && bits != 17 && bits != 21) continue;
      const std::vector<Chunk> chunks = CabChunks(c.data, bits);
      if (chunks.empty()) {
        std::printf("%s, window 2^%u: FCI gave no LZX folder\n", c.name, bits);
        ++g_failures;
        continue;
      }
      LzxDecoder decoder;
      CHECK(decoder.Init(bits));
      Bytes out;
      const bool decoded = DecodeAll(decoder, chunks, out);
      if (!decoded || out != c.data) {
        size_t first = 0;
        while (first < std::min(out.size(), c.data.size()) && out[first] == c.data[first]) ++first;
        std::printf("%s, window 2^%u: %s, %zu of %zu bytes, first difference at %zu\n", c.name, bits,
                    decoded ? "decoded" : "refused", out.size(), c.data.size(), first);
        ++g_failures;
      }
      ++checked;
      // The chunk header says whether the encoder translated E8 calls (the code cases).
      translated += chunks[0].data[1] >> 7;
      // A reset decoder gives the same output; so does one taken from a smaller-window use (Init again).
      decoder.Init(bits);
      CHECK(DecodeAll(decoder, chunks, out) && out == c.data);
    }
  }
  // Matches from the window's far edge (offset window - 3) right after short-offset matches: a random lap of
  // window - 3 bytes, then the same lap again with, every few hundred bytes, a 9-12 byte span copied from 20-40 bytes
  // before it. The bytes ahead of the write position are the previous lap's, still referenced: a match copy must
  // not write past its own end.
  for (uint32_t bits = 15; bits <= 21; ++bits) {
    if (bits == 18 || bits == 20) continue;
    const uint32_t lap = (1u << bits) - 3;
    const Bytes block = Random(lap, 40 + bits);
    std::mt19937 rng(bits);
    Bytes data(block);
    for (int laps = 0; laps < 2; ++laps) {
      size_t at = data.size();
      data.insert(data.end(), block.begin(), block.end());
      for (size_t i = at + 64 + rng() % 200; i + 12 < data.size(); i += 100 + rng() % 400) {
        const size_t span = 9 + rng() % 4, back = 20 + rng() % 21;
        for (size_t k = 0; k < span; ++k) data[i + k] = data[i + k - back];
      }
    }
    const std::vector<Chunk> chunks = CabChunks(data, bits);
    LzxDecoder decoder;
    CHECK(decoder.Init(bits));
    Bytes out;
    const bool decoded = DecodeAll(decoder, chunks, out);
    if (!decoded || out != data) {
      size_t first = 0;
      while (first < std::min(out.size(), data.size()) && out[first] == data[first]) ++first;
      std::printf("window-edge repeats, window 2^%u: %s, first difference at %zu of %zu\n", bits,
                  decoded ? "decoded" : "refused", first, data.size());
      ++g_failures;
    }
    ++checked;
  }
  CHECK(checked > 40);
  // The host's decoder (the benches' host column) gives the data back too.
  {
    any360::test::cab::Fdi fdi;
    const Bytes data = Mixed(300000, 21);
    Bytes out(data.size());
    CHECK(fdi.Decode(any360::test::cab::MakeCab(data, 17), out) && out == data);
  }
  std::printf("cab: %d streams decoded, %d with E8 translation\n", checked, translated);
}

#endif

// --- the LDI contexts ---

struct Calls {
  std::vector<std::pair<uint32_t, uint32_t>> log;  // (function, argument)
  uint32_t next = 0x40000;
  bool fail_window = false;
};

uint32_t FakeCall(uint32_t function, uint32_t argument, void* user) {
  auto& calls = *static_cast<Calls*>(user);
  calls.log.push_back({function, argument});
  if (function == 0xA11C) {
    if (calls.fail_window && argument != LdiContexts::kContextSize) return 0;
    const uint32_t at = calls.next;
    calls.next += (argument + 0xFFF) & ~0xFFFu;
    return at;
  }
  return 0;
}

void ContextTests() {
  Bytes memory(0x400000);
  const Guest g{memory.data()};
  constexpr uint32_t kAlloc = 0xA11C, kFree = 0xF4EE, io = 0x1000, config = 0x1100, scratch = 0x2000;
  Ldi().Reset();
  Calls calls;
  // Bad parameters; the source minimum with no handle pointer.
  g.Store32(io, 0x8000);
  g.Store32(config, 0x20000);
  CHECK(Ldi().Create(g, 0, config, 0, 0, scratch, io + 4, io + 8, FakeCall, &calls) == LdiContexts::kBadParameters);
  CHECK(Ldi().Create(g, io, 0, 0, 0, scratch, io + 4, io + 8, FakeCall, &calls) == LdiContexts::kBadParameters);
  CHECK(Ldi().Create(g, io, config, 0, 0, scratch, io + 4, 0, FakeCall, &calls) == 0 && g.Load32(io + 4) == 0x9800);
  CHECK(Ldi().size() == 0);
  // Window sizes: powers of two from 2^12 to 2^22.
  for (const uint32_t size : {0u, 0x800u, 0x3000u, 0x800000u}) {
    g.Store32(io + 8, 0xFFFFFFFF);
    g.Store32(config, size);
    CHECK(Ldi().Create(g, io, config, 0, 0, scratch, 0, io + 8, FakeCall, &calls) == LdiContexts::kNotEnoughMemory);
    CHECK(g.Load32(io + 8) == 0);
  }
  CHECK(calls.log.empty() && Ldi().size() == 0);
  // The scratch context: the handle is the scratch memory, which starts with "LDIC".
  g.Store32(config, 0x8000);
  CHECK(Ldi().Create(g, io, config, 0, 0, scratch, 0, io + 8, FakeCall, &calls) == 0);
  const uint32_t handle = g.Load32(io + 8);
  CHECK(handle == scratch && g.Load32(scratch) == LdiContexts::kMagic && Ldi().size() == 1);
  // A chunk: one uncompressed block of 100 bytes.
  const Bytes data = Random(100, 3);
  BitWriter w;
  w.Put(0, 1);
  UncompressedHeader(w, 100);
  w.Raw(data.data(), data.size());
  const uint32_t source = 0x10000, destination = 0x20000;
  std::memcpy(g.host(source), w.out.data(), w.out.size());
  g.Store32(io + 12, 100);
  CHECK(Ldi().Decompress(g, handle, source, uint32_t(w.out.size()), destination, io + 12) == 0);
  CHECK(g.Load32(io + 12) == 100 && std::memcmp(g.host(destination), data.data(), 100) == 0);
  // Sizes: 0 or past the data block max overflow (the size kept), past 32 KiB fails (the size cleared); an unknown
  // handle or no size pointer are bad parameters.
  g.Store32(io + 12, 0);
  CHECK(Ldi().Decompress(g, handle, source, 10, destination, io + 12) == LdiContexts::kBufferOverflow);
  g.Store32(io + 12, 0x8001);
  CHECK(Ldi().Decompress(g, handle, source, 10, destination, io + 12) == LdiContexts::kBufferOverflow &&
        g.Load32(io + 12) == 0x8001);
  CHECK(Ldi().Decompress(g, handle + 4, source, 10, destination, io + 12) == LdiContexts::kBadParameters);
  CHECK(Ldi().Decompress(g, handle, source, 10, destination, 0) == LdiContexts::kBadParameters);
  // A corrupt chunk fails with the size cleared; a wrapping range fails.
  g.Store32(io + 12, 50);
  std::memset(g.host(source), 0xFF, 16);
  CHECK(Ldi().Decompress(g, handle, source, 16, destination, io + 12) == LdiContexts::kFailed && g.Load32(io + 12) == 0);
  g.Store32(io + 12, 50);
  CHECK(Ldi().Decompress(g, handle, 0xFFFFFFF0, 0x20, destination, io + 12) == LdiContexts::kFailed);
  // A cleared magic makes the handle unknown.
  g.Store32(scratch, 0);
  g.Store32(io + 12, 50);
  CHECK(Ldi().Decompress(g, handle, source, 16, destination, io + 12) == LdiContexts::kBadParameters);
  CHECK(Ldi().Destroy(g, handle, FakeCall, &calls) == LdiContexts::kBadParameters);
  g.Store32(scratch, LdiContexts::kMagic);
  CHECK(Ldi().Destroy(g, handle, FakeCall, &calls) == 0 && g.Load32(scratch) == 0 && Ldi().size() == 0);
  CHECK(Ldi().Destroy(g, handle, FakeCall, &calls) == LdiContexts::kBadParameters);
  CHECK(calls.log.empty());

  // The title's allocator: the context, then the window and its margin; the freer: the window, then the context.
  g.Store32(config, 0x20000);
  CHECK(Ldi().Create(g, io, config, kAlloc, kFree, scratch, 0, io + 8, FakeCall, &calls) == 0);
  const uint32_t allocated = g.Load32(io + 8);
  CHECK(calls.log.size() == 2 && calls.log[0] == std::make_pair(kAlloc, LdiContexts::kContextSize) &&
        calls.log[1] == std::make_pair(kAlloc, 0x20000u + LdiContexts::kWindowExtra));
  CHECK(allocated == 0x40000 && g.Load32(allocated) == LdiContexts::kMagic && g.Load32(scratch) == 0);
  // Decoding through it, then destroying it.
  std::memcpy(g.host(source), w.out.data(), w.out.size());
  g.Store32(io + 12, 100);
  CHECK(Ldi().Decompress(g, allocated, source, uint32_t(w.out.size()), destination, io + 12) == 0);
  CHECK(Ldi().Destroy(g, allocated, FakeCall, &calls) == 0);
  const uint32_t window = 0x40000 + 0x3000;
  CHECK(calls.log.size() == 4 && calls.log[2] == std::make_pair(kFree, window) &&
        calls.log[3] == std::make_pair(kFree, allocated));
  // The window's allocation fails: the context is freed, nothing kept.
  calls.log.clear();
  calls.fail_window = true;
  const uint32_t context_at = calls.next;
  CHECK(Ldi().Create(g, io, config, kAlloc, kFree, scratch, 0, io + 8, FakeCall, &calls) == LdiContexts::kNotEnoughMemory);
  CHECK(calls.log.size() == 3 && calls.log[2] == std::make_pair(kFree, context_at));
  CHECK(g.Load32(io + 8) == 0 && Ldi().size() == 0);
  calls.fail_window = false;
  // A window the decoder does not cover (2^12): a context whose decompressions fail.
  g.Store32(config, 0x1000);
  CHECK(Ldi().Create(g, io, config, 0, 0, scratch, 0, io + 8, FakeCall, &calls) == 0);
  g.Store32(io + 12, 100);
  CHECK(Ldi().Decompress(g, scratch, source, uint32_t(w.out.size()), destination, io + 12) == LdiContexts::kFailed &&
        g.Load32(io + 12) == 0);
  CHECK(Ldi().Destroy(g, scratch, FakeCall, &calls) == 0);

  // Warm: create, decompress, destroy without a host allocation.
  g.Store32(config, 0x200000);
  for (int round = 0; round < 2; ++round) {
    const AllocCounts before = ThreadAllocations();
    for (int i = 0; i < 50; ++i) {
      CHECK(Ldi().Create(g, io, config, 0, 0, scratch, 0, io + 8, FakeCall, &calls) == 0);
      g.Store32(io + 12, 100);
      CHECK(Ldi().Decompress(g, scratch, source, uint32_t(w.out.size()), destination, io + 12) == 0);
      CHECK(Ldi().Destroy(g, scratch, FakeCall, &calls) == 0);
    }
    if (round) CHECK((ThreadAllocations() - before).count == 0);
  }
  Ldi().Reset();
  CHECK(Ldi().size() == 0);
}

}  // namespace

int main() {
  HandBuiltTests();
#if defined(_WIN32)
  CabTests();
#endif
  ContextTests();
  if (g_failures) {
    std::printf("sys_ldi_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("sys_ldi_tests: all passed\n");
  return 0;
}
