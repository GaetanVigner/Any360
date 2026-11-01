// The kernel side of the graphics: what the Vd* calls set up between a title and its GPU. The command ring buffer and
// its read-pointer write-back are handed to a GpuBackend (the command processor and renderer: any360::gfx, or the
// reference's GPU while stage 0 lasts); the title's interrupt callback is run on a guest context of its own at
// dispatch level, for vertical blanks (source 0) and command-processor interrupts (source 1) the backend raises.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "guest.h"
#include "scheduler.h"

namespace any360::sys {

class GpuBackend {
 public:
  virtual ~GpuBackend() = default;
  // The primary ring buffer: 2^size_log2 bytes at `base` (a physical-view address).
  virtual void InitializeRingBuffer(Guest g, uint32_t base, uint32_t size_log2) = 0;
  // Where the GPU writes its read pointer back, every 2^block_log2 bytes it consumes.
  virtual void EnableReadPointerWriteBack(Guest g, uint32_t address, uint32_t block_log2) = 0;
};

class GraphicsKernel {
 public:
  void Reset();
  void SetBackend(GpuBackend* backend) { backend_ = backend; }

  uint32_t InitializeEngines();
  void InitializeRingBuffer(Guest g, uint32_t base, uint32_t size_log2);
  void EnableReadPointerWriteBack(Guest g, uint32_t address, uint32_t block_log2);
  void SetInterruptCallback(Guest g, uint32_t callback, uint32_t user_data);
  // VdPersistDisplay: a 64-byte block the title later frees with MmFreePhysicalMemory; returns 1.
  uint32_t PersistDisplay(Guest g, uint32_t block_ptr);

  // Runs the title's interrupt callback for `source` (0 vertical blank, 1 command processor). Returns false when no
  // callback is set.
  bool RaiseInterrupt(Guest g, uint32_t source);
  // A host thread raising vertical-blank interrupts at `hz` until StopVblank.
  void StartVblank(double hz);
  void StopVblank();

  uint32_t ring_base() const { return ring_base_; }
  uint32_t ring_size_log2() const { return ring_size_log2_; }
  uint32_t write_back() const { return write_back_; }
  uint32_t interrupt_callback() const { return callback_; }

 private:
  GpuBackend* backend_ = nullptr;
  uint32_t ring_base_ = 0, ring_size_log2_ = 0, write_back_ = 0, write_back_block_log2_ = 0;
  uint32_t callback_ = 0, user_data_ = 0;
  GuestContext context_{};
  std::atomic<bool> vblank_running_{false};
  std::thread vblank_;
};

GraphicsKernel& Graphics();

}  // namespace any360::sys
