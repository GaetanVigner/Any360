// The audio group on any360::sys: the XMA decoder (any360_sys_xma) as the audio kernel's XmaBackend, and an SDL3
// output (the SDL the runtime links) as its sink. Built only when any360_sys_xma is (FFmpeg's headers found).
#include <SDL3/SDL.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mmio_handler.h>
#include <rex/system/xmemory.h>

#include <bit>

#include <chrono>
#include <string>
#include <thread>

#include "any360/host/system.h"
#include "any360/sys/audio.h"
#include "any360/sys/objects.h"
#include "any360/sys/xma.h"

namespace any360::host::rexglue {

namespace {

// The frames as 5.1 floats to the default playback device, SDL converting to its layout; Submit waits while more
// than a few frames are queued, so the audio clients run at the device's pace.
class SdlSink : public sys::AudioSink {
 public:
  bool Open(std::string& error) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      error = std::string("SDL audio: ") + SDL_GetError();
      return false;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, int(sys::kAudioChannels), 48000};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
      error = std::string("SDL audio device: ") + SDL_GetError();
      return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
  }

  void Submit(uint32_t client, const float* interleaved) override {
    // The hardware mixes its clients; titles register one (XAudio2's): the others' frames are dropped.
    if (owner_ == kNone) owner_ = client;
    if (client != owner_) return;
    constexpr int kFrameBytes = int(sizeof(float) * sys::kAudioFrameSamples * sys::kAudioChannels);
    SDL_PutAudioStreamData(stream_, interleaved, kFrameBytes);
    while (SDL_GetAudioStreamQueued(stream_) > kFrameBytes * kQueuedFrames) {
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
  }

 private:
  static constexpr uint32_t kNone = ~0u;
  static constexpr int kQueuedFrames = 6;  // ~32 ms
  SDL_AudioStream* stream_ = nullptr;
  uint32_t owner_ = kNone;
};

SdlSink* g_sink = nullptr;
sys::XmaDecoder* g_xma = nullptr;

// The XMA registers as the title's loads and stores reach them (big-endian words).
uint32_t ReadXmaRegister(void*, void*, uint32_t address) {
  return std::byteswap(sys::Audio().XmaReadRegister(sys::Objects().guest(), (address & 0xFFFF) / 4));
}
void WriteXmaRegister(void*, void*, uint32_t address, uint32_t value) {
  sys::Audio().XmaWriteRegister(sys::Objects().guest(), (address & 0xFFFF) / 4, std::byteswap(value));
}

}  // namespace

bool StartAudio(std::string& error) {
  if (g_sink) return true;
  if (!sys::XmaDecoder::Available()) {
    error = "the linked FFmpeg has no xmaframes decoder";
    return false;
  }
  auto* sink = new SdlSink;
  if (!sink->Open(error)) {
    delete sink;
    return false;
  }
  // The runtime's decoder answers the XMA registers its range maps: the range now reaches any360::sys's.
  rex::runtime::MMIORange* range = REX_KERNEL_STATE()->memory()->LookupVirtualMappedRange(0x7FEA0000);
  if (!range) {
    delete sink;
    error = "no XMA register range in the runtime's memory";
    return false;
  }
  range->callback_context = nullptr;
  range->read = &ReadXmaRegister;
  range->write = &WriteXmaRegister;
  g_sink = sink;
  g_xma = new sys::XmaDecoder;
  sys::Audio().SetXmaBackend(g_xma);
  sys::Audio().SetSink(g_sink);
  g_xma->Start(sys::Objects().guest());
  sys::Audio().Start();
  return true;
}

}  // namespace any360::host::rexglue
