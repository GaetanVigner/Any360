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

namespace any360::sys {

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
