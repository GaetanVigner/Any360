#include "any360/sys/audio.h"

#include <chrono>
#include <cstring>

#include "any360/sys/memory.h"
#include "any360/sys/objects.h"
#include "any360/sys/xam.h"

namespace any360::sys {

namespace {
constexpr uint32_t kEInvalidArg = 0x80070057, kDriverTag = 0x41550000;
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
  return 0;
}

}  // namespace any360::sys
