// The XMA decoder's context machinery (no XMA stream ships with the repository: decoding itself is checked in game):
// the codec is there, a clear resets a context, a kick with no input reports the stall titles look for, a packet
// with no frame start moves the decoder on to the other input buffer, a disabled context is left alone.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <random>
#include <vector>

#include "any360/sys/audio.h"
#include "any360/sys/xma.h"
#include "guest_memory.h"

using namespace any360::sys;

namespace {
int g_failures = 0;
#define CHECK(condition)                                                \
  do {                                                                  \
    if (!(condition)) {                                                 \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition);       \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

// The context's words, big-endian as the guest sees them.
uint32_t Word(Guest g, uint32_t context, uint32_t index) { return g.Load32(context + 4 * index); }
void SetWord(Guest g, uint32_t context, uint32_t index, uint32_t value) { g.Store32(context + 4 * index, value); }
}  // namespace

int main() {
  const Guest g{any360::test::GuestMemory::Get().base()};
  CHECK(XmaDecoder::Available());
#if ANY360_FFMPEG_X86ASM
  // FFmpeg built with its x86 assembly: its DSP code sees the processor's SIMD (unless switched off on purpose).
  if (!std::getenv("ANY360_XMA_NO_SIMD")) CHECK(XmaFrameDecoder::SimdFlags() != 0);
#endif
  std::printf("xma_tests: FFmpeg SIMD flags 0x%X\n", unsigned(XmaFrameDecoder::SimdFlags()));

  XmaDecoder decoder;
  const uint32_t context = 0xA0010000, output = 0x00020000, input0 = 0x00030000, input1 = 0x00040000;
  decoder.ContextAllocated(g, context);
  std::memset(g.host(context), 0, 64);

  // A clear: input invalid, read offset back to the first frame (bit 32), output offsets emptied.
  SetWord(g, context, 0, (1u << 20) | (1u << 21) | (3u << 27) | (2u << 22) | 5);
  SetWord(g, context, 1, 1u << 31);
  SetWord(g, context, 2, 12345);
  SetWord(g, context, 9, 3);
  decoder.ContextInitialized(g, context);
  CHECK((Word(g, context, 0) & ((1u << 20) | (1u << 21) | (31u << 27))) == 0);
  CHECK((Word(g, context, 0) & 0xFFF) == 5 && (Word(g, context, 0) >> 22 & 31) == 2);  // the title's fields stay
  CHECK((Word(g, context, 1) >> 31) == 0 && (Word(g, context, 2) & 0x03FFFFFF) == 32 && (Word(g, context, 9) & 31) == 0);

  // Output valid, no input: the kick makes write == read and drops the output's validity (the stall titles watch).
  SetWord(g, context, 0, 1u | (4u << 22) | (2u << 27));  // a packet (invalid), 4 output blocks, write offset 2
  SetWord(g, context, 1, (1u << 31) | (1u << 20));  // output valid, one subframe per decode
  SetWord(g, context, 7, output);
  SetWord(g, context, 9, 1);  // read offset 1
  decoder.ContextEnabled(g, context);
  CHECK((Word(g, context, 0) >> 27) == 1 && (Word(g, context, 1) >> 31) == 0);

  // A packet where no frame starts (skip count 0xFF), the only one of input buffer 0: the decoder moves to buffer 1.
  std::memset(g.host(input0), 0, 2048);
  g.Store8(input0 + 3, 0xFF);
  std::memset(g.host(input1), 0, 2048);
  SetWord(g, context, 0, 1u | (1u << 20) | (1u << 21) | (8u << 22));  // 1 packet, both inputs valid, 8 blocks
  SetWord(g, context, 1, (1u << 31) | (1u << 20) | 1u);
  SetWord(g, context, 2, 32);
  SetWord(g, context, 4, 0);
  SetWord(g, context, 5, input0);
  SetWord(g, context, 6, input1);
  SetWord(g, context, 9, 0);
  decoder.ContextEnabled(g, context);
  CHECK((Word(g, context, 0) >> 20 & 1) == 0);  // buffer 0 consumed
  CHECK((Word(g, context, 4) >> 31) == 1);      // buffer 1 current

  // Disabled, a context is not decoded however often the thread runs.
  SetWord(g, context, 0, 1u | (1u << 20) | (8u << 22));
  SetWord(g, context, 4, 0);
  decoder.ContextDisabled(g, context);
  CHECK(!decoder.WorkOnce());
  CHECK((Word(g, context, 0) >> 20 & 1) == 1);

  // The thread starts and stops; a released context is forgotten by the decoding.
  decoder.Start(g);
  decoder.ContextReleased(g, context);
  decoder.Stop();

  // The output conversion: four samples at a time against its scalar statement, every byte, mono and stereo, on
  // random samples and the edges (the clamp's bounds and just past them, infinities, NaNs, denormals, signed zeros).
  {
    std::mt19937 rng(0x58A1);
    std::uniform_real_distribution<float> wide(-1.5f, 1.5f);
    const float edges[] = {0.0f, -0.0f, 1.0f, -1.0f, std::nextafter(1.0f, 2.0f), std::nextafter(-1.0f, -2.0f),
                           std::nextafter(1.0f, 0.0f), 0.5f / 32767.0f, -0.5f / 32767.0f, 1e-40f, -1e-40f,
                           std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::quiet_NaN()};
    std::vector<float> left(512), right(512);
    std::vector<uint8_t> fast(2048), slow(2048);
    uint32_t compared = 0;
    for (int round = 0; round < 200; ++round) {
      for (uint32_t i = 0; i < 512; ++i) {
        left[i] = rng() % 4 ? wide(rng) : edges[rng() % std::size(edges)];
        right[i] = rng() % 4 ? wide(rng) : edges[rng() % std::size(edges)];
      }
      const float* planes[2] = {left.data(), right.data()};
      for (const bool stereo : {false, true}) {
        std::fill(fast.begin(), fast.end(), uint8_t(0xCD));
        std::fill(slow.begin(), slow.end(), uint8_t(0xCD));
        XmaFrameDecoder::Convert(planes, stereo, fast.data());
        XmaFrameDecoder::ConvertReference(planes, stereo, slow.data());
        CHECK(fast == slow);
        ++compared;
      }
    }
    CHECK(compared == 400);
  }

  std::printf("xma_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
