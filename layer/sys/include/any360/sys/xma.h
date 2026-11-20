// An XmaBackend that decodes: the XMA hardware's context machinery (input packets, frames spanning packets, loops,
// the output ring the title reads) on a host thread, each frame decoded by FFmpeg's xmaframes decoder (the FFmpeg the
// ReXGlue SDK ships). Kept out of any360_sys (target any360_sys_xma) so the core needs no FFmpeg.
//
// Ported from Xenia's XmaContext and XmaDecoder (BSD license, copyright Ben Vanik and the Xenia contributors; the
// ReXGlue adaptation by Tom Clay), itself crediting koolkdev's libertyv xma2dec.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "audio.h"
#include "guest.h"

extern "C" {
struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
}

namespace any360::sys {

// One XMA stream's frames through FFmpeg's xmaframes decoder, each frame's samples out as big-endian 16-bit,
// interleaved (512 a channel, saturated): what XmaDecoder writes to a title's output ring. XmaDecoder keeps one a
// context; xma_bench replays captured frames through one.
class XmaFrameDecoder {
 public:
  XmaFrameDecoder();
  ~XmaFrameDecoder();
  XmaFrameDecoder(const XmaFrameDecoder&) = delete;
  XmaFrameDecoder& operator=(const XmaFrameDecoder&) = delete;

  // A new logical stream: the next Prepare reopens the codec (the previous stream's overlap-add tail dropped).
  void Reset();
  // The codec for a stream of `rate_id` (0-3: 24, 32, 44.1, 48 kHz) and channels: 1 when (re)opened for it, 0 when
  // kept, -1 on failure.
  int Prepare(uint32_t rate_id, bool stereo);
  // One frame: `frame` holds a byte for the bit paddings (set here) then the frame's `frame_bits` bits, starting
  // `padding` bits into its first byte. Writes 512 samples a channel to `out`; false when FFmpeg drops the frame.
  bool Decode(uint8_t* frame, uint32_t frame_bits, uint32_t padding, bool stereo, uint8_t* out);

  // A frame's planar float samples (512 a channel) as big-endian 16-bit, interleaved: each clamped to [-1, 1], times
  // 32767, truncated toward zero, its low 16 bits kept (a NaN gives 0). Four samples at a time; the same bits as
  // ConvertReference, its scalar statement (both public for the tests).
  static void Convert(const float* const* planes, bool stereo, uint8_t* out);
  static void ConvertReference(const float* const* planes, bool stereo, uint8_t* out);

  // The SIMD extensions FFmpeg's DSP code uses on this processor (av_get_cpu_flags: 0 when FFmpeg was built without
  // its x86 assembly, or with ANY360_XMA_NO_SIMD=1, which forces its C versions for a same-build comparison).
  static int SimdFlags();

 private:
  AVPacket* packet_ = nullptr;
  const AVCodec* codec_ = nullptr;
  AVCodecContext* context_ = nullptr;
  AVFrame* frame_ = nullptr;
};

class XmaDecoder : public XmaBackend {
 public:
  XmaDecoder();
  ~XmaDecoder() override;

  // Ready to decode in the guest memory `g`. Decoding runs on each kick, on the kicking thread: a decode running behind
  // a title that takes XMAEnableContext's return as the output being ready would write into buffers it has reused.
  void Start(Guest g);
  void Stop();

  void ContextAllocated(Guest g, uint32_t context) override;
  void ContextReleased(Guest g, uint32_t context) override;
  // The hardware's kick: the context decodes as far as its input and output allow, before this returns.
  void ContextEnabled(Guest g, uint32_t context) override;
  // The hardware's lock: no decoding of the context once this returns.
  void ContextDisabled(Guest g, uint32_t context) override;
  // The hardware's clear (XMAInitializeContext ends with it): input invalid, offsets reset, decoder state dropped.
  void ContextInitialized(Guest g, uint32_t context) override;

  // Whether the FFmpeg linked in has the xmaframes decoder (without it every frame is dropped, silently).
  static bool Available();

  // Decodes what the enabled contexts allow once (the thread's step; tests call it directly). True when any worked.
  bool WorkOnce();

  struct Context;

 private:
  Context* Find(uint32_t context);

  Guest guest_{};
  std::mutex contexts_mutex_;
  std::map<uint32_t, std::unique_ptr<Context>> contexts_;
};

}  // namespace any360::sys
