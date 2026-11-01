#include "any360/sys/graphics.h"

#include <chrono>
#include <cstring>

#include "any360/sys/memory.h"
#include "any360/sys/objects.h"

namespace any360::sys {

namespace {
constexpr uint32_t kPrcbDpcActive = 0x100 + 0x50, kPcrIrql = 0x18;
}

GraphicsKernel& Graphics() {
  static GraphicsKernel graphics;
  return graphics;
}

void GraphicsKernel::Reset() {
  StopVblank();
  backend_ = nullptr;
  ring_base_ = ring_size_log2_ = write_back_ = write_back_block_log2_ = 0;
  callback_ = user_data_ = 0;
  context_ = {};
}

uint32_t GraphicsKernel::InitializeEngines() { return 1; }

void GraphicsKernel::InitializeRingBuffer(Guest g, uint32_t base, uint32_t size_log2) {
  ring_base_ = base;
  ring_size_log2_ = size_log2;
  if (backend_) backend_->InitializeRingBuffer(g, base, size_log2);
}

void GraphicsKernel::EnableReadPointerWriteBack(Guest g, uint32_t address, uint32_t block_log2) {
  write_back_ = address;
  write_back_block_log2_ = block_log2;
  if (backend_) backend_->EnableReadPointerWriteBack(g, address, block_log2);
}

void GraphicsKernel::SetInterruptCallback(Guest g, uint32_t callback, uint32_t user_data) {
  callback_ = callback;
  user_data_ = user_data;
  // The callback runs on a context of its own, as the kernel's interrupt handler does.
  if (callback && !context_.pcr) Threads().CreateContext(g, 0x10000, context_);
}

uint32_t GraphicsKernel::PersistDisplay(Guest g, uint32_t block_ptr) {
  if (block_ptr) g.Store32(block_ptr, Memory().AllocatePhysical(g, 0, 64, 0x4 /* PAGE_READWRITE */, 0, 0xFFFFFFFF, 32));
  return 1;
}

bool GraphicsKernel::RaiseInterrupt(Guest g, uint32_t source) {
  if (!callback_ || !context_.pcr) return false;
  // At dispatch level, as a DPC: the title's handler checks this through KeGetCurrentProcessType and the IRQL.
  g.Store32(context_.pcr + kPrcbDpcActive, 1);
  g.Store8(context_.pcr + kPcrIrql, 2);
  Threads().Dispatch(callback_, context_, source, user_data_);
  g.Store8(context_.pcr + kPcrIrql, 0);
  g.Store32(context_.pcr + kPrcbDpcActive, 0);
  return true;
}

void GraphicsKernel::StartVblank(double hz) {
  StopVblank();
  vblank_running_ = true;
  vblank_ = std::thread([this, hz] {
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = std::chrono::steady_clock::now() + period;
    while (vblank_running_) {
      std::this_thread::sleep_until(next);
      next += period;
      RaiseInterrupt(Objects().guest(), 0);
    }
  });
}

void GraphicsKernel::StopVblank() {
  vblank_running_ = false;
  if (vblank_.joinable()) vblank_.join();
}

}  // namespace any360::sys
