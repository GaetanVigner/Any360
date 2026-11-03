#include "any360/sys/audio.h"

#include <chrono>
#include <cstring>
#include <iterator>

#include "any360/sys/memory.h"
#include "any360/sys/objects.h"
#include "any360/sys/xam.h"

namespace any360::sys {

namespace {
constexpr uint32_t kEInvalidArg = 0x80070057, kDriverTag = 0x41550000, kXmaFalse = 0x80000000;

struct XmaBits {
  uint32_t word, shift, width;
};
// Indexed by XmaField.
constexpr XmaBits kXmaBits[] = {
    {0, 0, 12}, {0, 12, 8},  {0, 20, 1},  {0, 21, 1},  {0, 22, 5}, {0, 27, 5}, {1, 0, 12}, {1, 14, 3},
    {1, 17, 3}, {1, 20, 4},  {1, 27, 2},  {1, 29, 1},  {1, 31, 1}, {2, 0, 26}, {3, 0, 26}, {4, 0, 26},
    {4, 26, 5}, {5, 0, 32},  {6, 0, 32},  {7, 0, 32},  {8, 0, 32}, {9, 0, 5},
};
static_assert(std::size(kXmaBits) == size_t(XmaField::kOutputReadOffset) + 1);

uint32_t Mask(uint32_t width) { return width == 32 ? 0xFFFFFFFF : (1u << width) - 1; }

// A guest address as the decoder sees it; 0 stays 0 (an unused input buffer).
bool ToPhysical(uint32_t address, uint32_t& physical) {
  physical = address ? Memory().PhysicalAddress(address) : 0;
  return physical != 0xFFFFFFFF;
}
}

AudioKernel& Audio() {
  static AudioKernel audio;
  return audio;
}

void AudioKernel::Reset() {
  Stop();
  sink_ = nullptr;
  xma_ = nullptr;
  clients_ = {};
  context_ = {};
  xma_base_ = 0;
  xma_used_.clear();
  xma_enabled_.clear();
}

uint32_t AudioKernel::RegisterClient(Guest g, uint32_t callback_ptr, uint32_t driver_ptr) {
  if (!callback_ptr) return kEInvalidArg;
  const uint32_t callback = g.Load32(callback_ptr), argument = g.Load32(callback_ptr + 4);
  if (!callback) return kEInvalidArg;
  for (uint32_t i = 0; i < kAudioMaxClients; ++i) {
    if (clients_[i].used) continue;
    clients_[i] = {true, callback, argument};
    if (!context_.pcr) Threads().CreateContext(g, 0x10000, context_);
    if (driver_ptr) g.Store32(driver_ptr, kDriverTag | i);
    return kErrorSuccess;
  }
  return kEInvalidArg;
}

uint32_t AudioKernel::UnregisterClient(uint32_t driver) {
  const uint32_t i = driver & 0xFFFF;
  if ((driver & 0xFFFF0000) != kDriverTag || i >= kAudioMaxClients) return kEInvalidArg;
  clients_[i] = {};
  return kErrorSuccess;
}

uint32_t AudioKernel::SubmitFrame(Guest g, uint32_t driver, uint32_t samples) {
  const uint32_t i = driver & 0xFFFF;
  if ((driver & 0xFFFF0000) != kDriverTag || i >= kAudioMaxClients || !clients_[i].used) return kEInvalidArg;
  if (!sink_) return kErrorSuccess;
  // Guest: channel after channel, big-endian. Host: sample after sample, native floats.
  float frame[kAudioFrameSamples * kAudioChannels];
  for (uint32_t c = 0; c < kAudioChannels; ++c) {
    for (uint32_t s = 0; s < kAudioFrameSamples; ++s) {
      const uint32_t bits = g.Load32(samples + 4 * (c * kAudioFrameSamples + s));
      std::memcpy(&frame[s * kAudioChannels + c], &bits, 4);
    }
  }
  sink_->Submit(i, frame);
  return kErrorSuccess;
}

uint32_t AudioKernel::GetVoiceCategoryVolumeChangeMask(Guest g, uint32_t, uint32_t out) {
  if (out) g.Store32(out, 0);  // no category volume changed
  return kErrorSuccess;
}

uint32_t AudioKernel::GetSpeakerConfig(Guest g, uint32_t out) {
  if (out) g.Store32(out, 0x00010001);
  return kErrorSuccess;
}

uint32_t AudioKernel::GetVoiceCategoryVolume(Guest g, uint32_t out) {
  const float one = 1.0f;
  uint32_t bits;
  std::memcpy(&bits, &one, 4);
  if (out) g.Store32(out, bits);
  return kErrorSuccess;
}

void AudioKernel::Pump(Guest) {
  for (const Client& c : clients_) {
    if (c.used) Threads().Dispatch(c.callback, context_, c.argument, 0);
  }
}

void AudioKernel::Start() {
  Stop();
  running_ = true;
  worker_ = std::thread([this] {
    const auto period = std::chrono::microseconds(5333);  // 256 samples at 48 kHz
    auto next = std::chrono::steady_clock::now();
    while (running_) {
      next += period;
      Pump(Objects().guest());
      std::this_thread::sleep_until(next);
    }
  });
}

void AudioKernel::Stop() {
  running_ = false;
  if (worker_.joinable()) worker_.join();
}

uint32_t AudioKernel::XmaCreateContext(Guest g, uint32_t context_out) {
  if (!xma_base_) {
    // The decoder's contexts: one physically contiguous block, as the hardware reads them.
    xma_base_ = Memory().AllocatePhysical(g, 0, kXmaContextSize * kXmaContexts, 0x4, 0, 0xFFFFFFFF, 256);
    if (!xma_base_) {
      if (context_out) g.Store32(context_out, 0);
      return kStatusNoMemory;
    }
    xma_used_.assign(kXmaContexts, false);
    xma_enabled_.assign(kXmaContexts, false);
  }
  for (uint32_t i = 0; i < kXmaContexts; ++i) {
    if (xma_used_[i]) continue;
    xma_used_[i] = true;
    const uint32_t context = xma_base_ + i * kXmaContextSize;
    std::memset(g.host(context), 0, kXmaContextSize);
    if (context_out) g.Store32(context_out, context);
    if (xma_) xma_->ContextAllocated(g, context);
    return 0;
  }
  if (context_out) g.Store32(context_out, 0);
  return kStatusNoMemory;
}

uint32_t AudioKernel::XmaReleaseContext(Guest g, uint32_t context) {
  if (!xma_base_ || context < xma_base_ || context >= xma_base_ + kXmaContextSize * kXmaContexts) return 0;
  const uint32_t i = (context - xma_base_) / kXmaContextSize;
  if (!xma_used_[i]) return 0;
  if (xma_) xma_->ContextReleased(g, context);
  std::memset(g.host(context), 0, kXmaContextSize);
  xma_used_[i] = false;
  xma_enabled_[i] = false;
  return 0;
}

int AudioKernel::XmaIndex(uint32_t context) const {
  if (!xma_base_ || context < xma_base_ || context >= xma_base_ + kXmaContextSize * kXmaContexts) return -1;
  return int((context - xma_base_) / kXmaContextSize);
}

uint32_t AudioKernel::XmaGet(Guest g, uint32_t context, XmaField field) const {
  const XmaBits bits = kXmaBits[size_t(field)];
  return (g.Load32(context + 4 * bits.word) >> bits.shift) & Mask(bits.width);
}

void AudioKernel::XmaSet(Guest g, uint32_t context, XmaField field, uint32_t value) const {
  const XmaBits bits = kXmaBits[size_t(field)];
  const uint32_t mask = Mask(bits.width) << bits.shift, address = context + 4 * bits.word;
  g.Store32(address, (g.Load32(address) & ~mask) | ((value << bits.shift) & mask));
}

uint32_t AudioKernel::XmaInitializeContext(Guest g, uint32_t context, uint32_t init) {
  uint32_t input0 = 0, input1 = 0, output = 0;
  if (!ToPhysical(g.Load32(init), input0) || !ToPhysical(g.Load32(init + 8), input1) ||
      !ToPhysical(g.Load32(init + 0x14), output) || !output) {
    return kXmaFalse;
  }
  std::memset(g.host(context), 0, kXmaContextSize);
  XmaSet(g, context, XmaField::kInput0Buffer, input0);
  XmaSet(g, context, XmaField::kInput0PacketCount, g.Load32(init + 4));
  XmaSet(g, context, XmaField::kInput1Buffer, input1);
  XmaSet(g, context, XmaField::kInput1PacketCount, g.Load32(init + 0xC));
  XmaSet(g, context, XmaField::kInputReadOffset, g.Load32(init + 0x10));
  XmaSet(g, context, XmaField::kOutputBuffer, output);
  XmaSet(g, context, XmaField::kOutputBlockCount, g.Load32(init + 0x18));
  XmaSet(g, context, XmaField::kSubframeDecodeCount, g.Load32(init + 0x20));
  XmaSet(g, context, XmaField::kStereo, g.Load32(init + 0x24) >= 1);  // as the reference: a channel count of 1+
  XmaSet(g, context, XmaField::kSampleRate, g.Load32(init + 0x28));
  XmaSetLoopData(g, context, init + 0x2C);
  return 0;
}

uint32_t AudioKernel::XmaSetInputBuffer(Guest g, uint32_t context, uint32_t index, uint32_t buffer, uint32_t packets) {
  uint32_t physical = 0;
  if (!ToPhysical(buffer, physical)) return kXmaFalse;
  XmaSet(g, context, index ? XmaField::kInput1Buffer : XmaField::kInput0Buffer, physical);
  XmaSet(g, context, index ? XmaField::kInput1PacketCount : XmaField::kInput0PacketCount, packets);
  return 0;
}

uint32_t AudioKernel::XmaSetLoopData(Guest g, uint32_t context, uint32_t loop) {
  XmaSet(g, context, XmaField::kLoopStart, g.Load32(loop));
  XmaSet(g, context, XmaField::kLoopEnd, g.Load32(loop + 4));
  XmaSet(g, context, XmaField::kLoopCount, g.Load8(loop + 8));
  XmaSet(g, context, XmaField::kLoopSubframeEnd, g.Load8(loop + 9));
  XmaSet(g, context, XmaField::kLoopSubframeSkip, g.Load8(loop + 10));
  return 0;
}

uint32_t AudioKernel::XmaEnableContext(Guest g, uint32_t context) {
  if (const int i = XmaIndex(context); i >= 0) xma_enabled_[i] = true;
  if (xma_) xma_->ContextEnabled(g, context);
  return 0;
}

uint32_t AudioKernel::XmaDisableContext(Guest g, uint32_t context) {
  if (const int i = XmaIndex(context); i >= 0) xma_enabled_[i] = false;
  if (xma_) xma_->ContextDisabled(g, context);
  return 0;
}

bool AudioKernel::XmaEnabled(uint32_t context) const {
  const int i = XmaIndex(context);
  return i >= 0 && xma_enabled_[i];
}

uint32_t AudioKernel::XmaBlockWhileInUse(Guest g, uint32_t context) const {
  while ((XmaGet(g, context, XmaField::kInput0Valid) || XmaGet(g, context, XmaField::kInput1Valid)) &&
         XmaGet(g, context, XmaField::kWorkBuffer)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return 0;
}

}  // namespace any360::sys
