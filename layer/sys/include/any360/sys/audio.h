// The kernel side of the audio: the render driver clients a title registers (its mixer callback, called every frame of
// 256 samples, 5.33 ms at 48 kHz), the frames it submits back (6 channels of 256 big-endian floats, channel after
// channel) handed to an AudioSink as host floats interleaved by sample, and the XMA decoder contexts (64-byte blocks
// in physical memory the hardware decoder works on; decoding them is the XmaBackend's job).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "guest.h"
#include "scheduler.h"

namespace any360::sys {

constexpr uint32_t kAudioChannels = 6, kAudioFrameSamples = 256, kAudioMaxClients = 8, kXmaContextSize = 64, kXmaContexts = 320;

class AudioSink {
 public:
  virtual ~AudioSink() = default;
  // One frame: kAudioFrameSamples samples of kAudioChannels interleaved floats (front L, R, centre, LFE, rear L, R).
  virtual void Submit(uint32_t client, const float* interleaved) = 0;
};

class XmaBackend {
 public:
  virtual ~XmaBackend() = default;
  // Called for each context the title allocates and releases; decoding follows the context's registers.
  virtual void ContextAllocated(Guest g, uint32_t context) = 0;
  virtual void ContextReleased(Guest g, uint32_t context) = 0;
  // XMAEnableContext / XMADisableContext: the decoder may start on the context, or must leave it before returning.
  virtual void ContextEnabled(Guest, uint32_t) {}
  virtual void ContextDisabled(Guest, uint32_t) {}
};

// The fields of an XMA context the XMA* calls read and write: big-endian words, bitfields from the low bit.
enum class XmaField {
  kInput0PacketCount, kLoopCount, kInput0Valid, kInput1Valid, kOutputBlockCount, kOutputWriteOffset,
  kInput1PacketCount, kLoopSubframeEnd, kLoopSubframeSkip, kSubframeDecodeCount, kSampleRate, kStereo,
  kOutputValid, kInputReadOffset, kLoopStart, kLoopEnd, kPacketMetadata, kInput0Buffer, kInput1Buffer,
  kOutputBuffer, kWorkBuffer, kOutputReadOffset,
};

class AudioKernel {
 public:
  void Reset();
  void SetSink(AudioSink* sink) { sink_ = sink; }
  void SetXmaBackend(XmaBackend* xma) { xma_ = xma; }

  // XAudioRegisterRenderDriverClient: the callback and its argument at callback_ptr; the driver handle written.
  uint32_t RegisterClient(Guest g, uint32_t callback_ptr, uint32_t driver_ptr);
  uint32_t UnregisterClient(uint32_t driver);
  uint32_t SubmitFrame(Guest g, uint32_t driver, uint32_t samples);
  uint32_t GetVoiceCategoryVolumeChangeMask(Guest g, uint32_t driver, uint32_t out);
  uint32_t GetVoiceCategoryVolume(Guest g, uint32_t out);
  // XAudioGetSpeakerConfig: the console's audio settings as the reference answers them (0x00010001).
  uint32_t GetSpeakerConfig(Guest g, uint32_t out);

  // Calls every registered client's callback once (the title then submits its next frame).
  void Pump(Guest g);
  // A host thread pumping at the frame rate until Stop.
  void Start();
  void Stop();

  uint32_t XmaCreateContext(Guest g, uint32_t context_out);
  uint32_t XmaReleaseContext(Guest g, uint32_t context);
  uint32_t XmaGet(Guest g, uint32_t context, XmaField field) const;
  void XmaSet(Guest g, uint32_t context, XmaField field, uint32_t value) const;
  // XMAInitializeContext: the context cleared and filled from the 56-byte init block (buffers as physical
  // addresses); 0x80000000 when a buffer is not in physical memory.
  uint32_t XmaInitializeContext(Guest g, uint32_t context, uint32_t init);
  // XMASetInputBuffer0/1: the buffer's physical address and its 2 KB packet count.
  uint32_t XmaSetInputBuffer(Guest g, uint32_t context, uint32_t index, uint32_t buffer, uint32_t packets);
  // XMASetLoopData from a 12-byte XMA_LOOP_DATA.
  uint32_t XmaSetLoopData(Guest g, uint32_t context, uint32_t loop);
  uint32_t XmaEnableContext(Guest g, uint32_t context);
  uint32_t XmaDisableContext(Guest g, uint32_t context);
  bool XmaEnabled(uint32_t context) const;
  // XMABlockWhileInUse: returns once neither input buffer is valid or the decoder has no work buffer.
  uint32_t XmaBlockWhileInUse(Guest g, uint32_t context) const;

 private:
  struct Client {
    bool used = false;
    uint32_t callback = 0, argument = 0;
  };
  int XmaIndex(uint32_t context) const;  // -1 for an address outside the decoder's contexts

  AudioSink* sink_ = nullptr;
  XmaBackend* xma_ = nullptr;
  std::array<Client, kAudioMaxClients> clients_{};
  GuestContext context_{};
  std::atomic<bool> running_{false};
  std::thread worker_;
  uint32_t xma_base_ = 0;
  std::vector<bool> xma_used_, xma_enabled_;
};

AudioKernel& Audio();

}  // namespace any360::sys
