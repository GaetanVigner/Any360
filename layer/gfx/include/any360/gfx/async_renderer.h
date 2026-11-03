// A renderer on a thread of its own: the command processor's calls (draws, resolves, swaps, fences...) queued in
// order with the registers they read, and run by a worker thread on the target renderer. The command processor
// reads the stream while the renderer turns the previous draws into GPU work.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "any360/gfx/command_processor.h"

namespace any360::gfx {

class AsyncRenderer : public Renderer {
 public:
  // `target` runs on the worker thread from now on (only there). `queue_bytes`: room for the calls in flight.
  AsyncRenderer(CommandProcessor& cp, Renderer& target, size_t queue_bytes = size_t(64) << 20);
  ~AsyncRenderer() override;  // runs what is queued, then stops the worker

  // Returns once the worker has run every call queued so far.
  void Drain();
  // Time the worker spent running calls (nanoseconds, summed): the renderer's thread's load.
  uint64_t busy_ns() const { return busy_ns_.load(std::memory_order_relaxed); }
  std::thread::native_handle_type worker_handle() { return worker_.native_handle(); }
  uint64_t queued_bytes() const { return published_.load(std::memory_order_relaxed); }  // all calls so far

  void ShaderLoaded(const Shader& shader) override;
  void Draw(const DrawCall& draw) override;
  void Copy(const uint32_t* registers) override;
  void Swap(uint32_t frontbuffer, uint32_t width, uint32_t height, const uint32_t* registers) override;
  void Flush() override;
  void WhenDone(std::function<void()> action) override;
  void Invalidate(uint32_t base, uint32_t size) override;
  void GammaRamp(const uint32_t* table, const uint32_t* pwl) override;

 private:
  enum class Op : uint32_t { kWrap, kRegisters, kShaderLoaded, kDraw, kCopy, kSwap, kFlush, kWhenDone, kInvalidate, kGammaRamp };
  struct Header {
    Op op;
    uint32_t bytes;  // the payload's, after the header
  };
  // Room for a command of `bytes` (header included, 8-byte aligned) at the producer's position; the caller fills it
  // and calls Publish.
  uint8_t* Reserve(uint32_t bytes);
  void Publish(uint8_t* at, Op op, uint32_t payload_bytes);
  void SendRegisters();  // the register blocks written since the last call that reads them
  const Shader* Stable(const Shader* shader);  // a copy that stays while the worker may use it
  void Run();
  void Execute(Op op, const uint8_t* payload, uint32_t bytes);

  CommandProcessor& cp_;
  Renderer& target_;
  std::vector<uint8_t> queue_;
  size_t write_ = 0;                  // producer position (bytes)
  std::atomic<size_t> published_{0};  // bytes published (monotonic)
  std::atomic<size_t> consumed_{0};   // bytes the worker is done with (monotonic)
  size_t produced_ = 0;               // published_, the producer's copy
  std::atomic<bool> stopping_{false}, worker_waiting_{false};
  std::atomic<uint64_t> busy_ns_{0};
  std::mutex mutex_;
  std::condition_variable work_, room_;
  std::thread worker_;
  std::vector<uint32_t> shadow_;  // the worker's register file
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;  // by type and hash (the producer's)
};

}  // namespace any360::gfx
