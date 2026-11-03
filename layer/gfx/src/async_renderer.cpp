#include "any360/gfx/async_renderer.h"

#include <bit>
#include <chrono>
#include <cstring>

namespace any360::gfx {

namespace {
constexpr uint32_t kBlockWords = CommandProcessor::kBlockRegisters;
constexpr uint32_t kBlocks = CommandProcessor::kDirtyWords * 64;
constexpr uint32_t Align8(uint32_t bytes) { return (bytes + 7) & ~7u; }

struct DrawPayload {
  uint32_t primitive_type, index_count, source, index_32, major_mode_explicit, edram_mode, index_base, index_bytes,
      index_endian, immediate_count;
  const Shader* vertex_shader;
  const Shader* pixel_shader;
};
}  // namespace

AsyncRenderer::AsyncRenderer(CommandProcessor& cp, Renderer& target, size_t queue_bytes)
    : cp_(cp), target_(target), queue_((queue_bytes + 7) & ~size_t(7)), shadow_(size_t(kBlocks) * kBlockWords) {
  // The worker starts from the register file as it is now.
  std::memcpy(shadow_.data(), cp_.registers(), reg::kRegisterCount * 4);
  uint64_t taken[CommandProcessor::kDirtyWords];
  cp_.TakeDirtyBlocks(taken);
  worker_ = std::thread([this] { Run(); });
}

AsyncRenderer::~AsyncRenderer() {
  stopping_ = true;
  {
    std::lock_guard lock(mutex_);
  }
  work_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void AsyncRenderer::Drain() {
  for (uint32_t spins = 0; consumed_.load(std::memory_order_acquire) != produced_; ++spins) {
    if (spins < 1000) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
}

uint8_t* AsyncRenderer::Reserve(uint32_t bytes) {
  const size_t size = queue_.size();
  auto wait_for_room = [&](size_t need) {
    if (produced_ + need - consumed_seen_ <= size) return;  // room, without reading the worker's line
    for (uint32_t spins = 0; produced_ + need - (consumed_seen_ = consumed_.load(std::memory_order_acquire)) > size; ++spins) {
      if (spins < 1000) {
        std::this_thread::yield();
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
    }
  };
  // A command never wraps: the rest of the queue is skipped with a wrap marker when it does not fit.
  const size_t at = write_ % size;
  if (size - at < bytes) {
    wait_for_room(size - at);
    Publish(queue_.data() + at, Op::kWrap, uint32_t(size - at - sizeof(Header)));
  }
  wait_for_room(bytes);
  return queue_.data() + write_ % size;
}

void AsyncRenderer::Publish(uint8_t* at, Op op, uint32_t payload_bytes) {
  const Header header{op, payload_bytes};
  std::memcpy(at, &header, sizeof(header));
  const uint32_t bytes = Align8(uint32_t(sizeof(Header)) + payload_bytes);
  write_ += bytes;
  produced_ += bytes;
  // A plain release store (a full barrier at every call stalls this thread on its pending stores): the worker going
  // to sleep just then may miss it, and then looks again within a millisecond (the next call wakes it anyway).
  published_.store(produced_, std::memory_order_release);
  // Wake the worker only when it sleeps (it checks again under the lock before sleeping).
  if (worker_waiting_.load(std::memory_order_relaxed)) {
    std::lock_guard lock(mutex_);
    work_.notify_one();
  }
}

void AsyncRenderer::SendRegisters() {
  uint64_t dirty[CommandProcessor::kDirtyWords];
  cp_.TakeDirtyBlocks(dirty);
  // Runs of consecutive written blocks, found by set bits (most draws write a few blocks).
  uint32_t runs[kBlocks][2];
  uint32_t run_count = 0, payload = 0;
  for (uint32_t w = 0; w < CommandProcessor::kDirtyWords; ++w) {
    for (uint64_t bits = dirty[w]; bits;) {
      const uint32_t first = std::countr_zero(bits);
      const uint32_t length = std::countr_one(bits >> first);  // < 64 ones from first, or to the word's end
      const uint32_t block = 64 * w + first;
      if (run_count && runs[run_count - 1][0] + runs[run_count - 1][1] == block) {
        runs[run_count - 1][1] += length;  // continues a run from the previous word
      } else {
        runs[run_count][0] = block, runs[run_count][1] = length, ++run_count;
      }
      payload += length * kBlockWords * 4;
      bits = first + length >= 64 ? 0 : bits & ~(((uint64_t(1) << length) - 1) << first);
    }
  }
  if (!run_count) return;
  payload += 8 * run_count;
  uint8_t* at = Reserve(Align8(uint32_t(sizeof(Header)) + payload));
  uint8_t* out = at + sizeof(Header);
  const uint32_t* registers = cp_.registers();
  for (uint32_t r = 0; r < run_count; ++r) {
    std::memcpy(out, runs[r], 8);
    out += 8;
    // The file's last block runs past its end: what is beyond stays zero.
    const uint32_t first = runs[r][0] * kBlockWords, words = runs[r][1] * kBlockWords;
    const uint32_t copied = first >= reg::kRegisterCount ? 0 : std::min(words, reg::kRegisterCount - first);
    std::memcpy(out, registers + first, size_t(copied) * 4);
    if (copied < words) std::memset(out + size_t(copied) * 4, 0, size_t(words - copied) * 4);
    out += size_t(words) * 4;
  }
  Publish(at, Op::kRegisters, payload);
}

const Shader* AsyncRenderer::Stable(const Shader* shader) {
  if (!shader) return nullptr;
  auto& slot = shaders_[shader->hash ^ (uint64_t(shader->type & 1) << 63)];
  if (!slot) slot = std::make_unique<Shader>(*shader);
  return slot.get();
}

void AsyncRenderer::ShaderLoaded(const Shader& shader) {
  // Kept (a load per draw is common, mostly the same shaders): the target gets each with the draws that use it.
  Stable(&shader);
}

void AsyncRenderer::Draw(const DrawCall& draw) {
  SendRegisters();
  const DrawPayload p{draw.primitive_type, draw.index_count, uint32_t(draw.source), draw.index_32, draw.major_mode_explicit,
                      draw.edram_mode, draw.index_base, draw.index_bytes, uint32_t(draw.index_endian),
                      uint32_t(draw.immediate.size()), Stable(draw.vertex_shader), Stable(draw.pixel_shader)};
  const uint32_t payload = uint32_t(sizeof(p) + draw.immediate.size() * 4);
  uint8_t* at = Reserve(Align8(uint32_t(sizeof(Header)) + payload));
  std::memcpy(at + sizeof(Header), &p, sizeof(p));
  if (!draw.immediate.empty()) std::memcpy(at + sizeof(Header) + sizeof(p), draw.immediate.data(), draw.immediate.size() * 4);
  Publish(at, Op::kDraw, payload);
}

void AsyncRenderer::Copy(const uint32_t*) {
  SendRegisters();
  Publish(Reserve(sizeof(Header)), Op::kCopy, 0);
}

void AsyncRenderer::Swap(uint32_t frontbuffer, uint32_t width, uint32_t height, const uint32_t*) {
  SendRegisters();
  const uint32_t p[3] = {frontbuffer, width, height};
  uint8_t* at = Reserve(Align8(sizeof(Header) + sizeof(p)));
  std::memcpy(at + sizeof(Header), p, sizeof(p));
  Publish(at, Op::kSwap, sizeof(p));
}

void AsyncRenderer::Flush() { Publish(Reserve(sizeof(Header)), Op::kFlush, 0); }

void AsyncRenderer::WhenDone(std::function<void()> action) {
  auto* held = new std::function<void()>(std::move(action));
  uint8_t* at = Reserve(Align8(sizeof(Header) + sizeof(held)));
  std::memcpy(at + sizeof(Header), &held, sizeof(held));
  Publish(at, Op::kWhenDone, sizeof(held));
}

void AsyncRenderer::Invalidate(uint32_t base, uint32_t size) {
  const uint32_t p[2] = {base, size};
  uint8_t* at = Reserve(Align8(sizeof(Header) + sizeof(p)));
  std::memcpy(at + sizeof(Header), p, sizeof(p));
  Publish(at, Op::kInvalidate, sizeof(p));
}

void AsyncRenderer::GammaRamp(const uint32_t* table, const uint32_t* pwl) {
  const uint32_t payload = (256 + 128 * 3) * 4;
  uint8_t* at = Reserve(Align8(sizeof(Header) + payload));
  std::memcpy(at + sizeof(Header), table, 256 * 4);
  std::memcpy(at + sizeof(Header) + 256 * 4, pwl, 128 * 3 * 4);
  Publish(at, Op::kGammaRamp, payload);
}

void AsyncRenderer::Run() {
  const size_t size = queue_.size();
  size_t position = 0;
  for (;;) {
    if (published_.load(std::memory_order_acquire) == position) {
      // Nothing queued: a short spin (the next call usually follows soon), then sleep until published.
      bool ready = false;
      for (uint32_t spins = 0; spins < 2000 && !ready; ++spins) {
        ready = published_.load(std::memory_order_acquire) != position;
        if (!ready) std::this_thread::yield();
      }
      if (!ready) {
        std::unique_lock lock(mutex_);
        worker_waiting_.store(true, std::memory_order_seq_cst);
        while (!work_.wait_for(lock, std::chrono::milliseconds(1),
                               [&] { return published_.load(std::memory_order_seq_cst) != position || stopping_; })) {
        }
        worker_waiting_.store(false, std::memory_order_seq_cst);
      }
      if (published_.load(std::memory_order_acquire) == position) {
        if (stopping_) return;
        continue;
      }
    }
    // Everything published, at once.
    const auto start = std::chrono::steady_clock::now();
    for (const size_t end = published_.load(std::memory_order_acquire); position != end;) {
      Header header;
      const uint8_t* at = queue_.data() + position % size;
      std::memcpy(&header, at, sizeof(header));
      Execute(header.op, at + sizeof(Header), header.bytes);
      position += Align8(uint32_t(sizeof(Header)) + header.bytes);
      consumed_.store(position, std::memory_order_release);
    }
    busy_ns_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
                       std::memory_order_relaxed);
  }
}

void AsyncRenderer::Execute(Op op, const uint8_t* payload, uint32_t bytes) {
  switch (op) {
    case Op::kWrap:
      break;
    case Op::kRegisters: {
      for (const uint8_t* p = payload; p < payload + bytes;) {
        uint32_t run[2];
        std::memcpy(run, p, 8);
        const size_t words = size_t(run[1]) * kBlockWords;
        std::memcpy(shadow_.data() + size_t(run[0]) * kBlockWords, p + 8, words * 4);
        p += 8 + words * 4;
      }
      break;
    }
    case Op::kShaderLoaded: {
      const Shader* shader;
      std::memcpy(&shader, payload, sizeof(shader));
      target_.ShaderLoaded(*shader);
      break;
    }
    case Op::kDraw: {
      DrawPayload p;
      std::memcpy(&p, payload, sizeof(p));
      DrawCall draw;
      draw.primitive_type = p.primitive_type;
      draw.index_count = p.index_count;
      draw.source = DrawSource(p.source);
      draw.index_32 = p.index_32;
      draw.major_mode_explicit = p.major_mode_explicit;
      draw.edram_mode = p.edram_mode;
      draw.index_base = p.index_base;
      draw.index_bytes = p.index_bytes;
      draw.index_endian = Endian(p.index_endian);
      if (p.immediate_count) {
        draw.immediate.resize(p.immediate_count);
        std::memcpy(draw.immediate.data(), payload + sizeof(p), size_t(p.immediate_count) * 4);
      }
      draw.vertex_shader = p.vertex_shader;
      draw.pixel_shader = p.pixel_shader;
      draw.registers = shadow_.data();
      target_.Draw(draw);
      break;
    }
    case Op::kCopy:
      target_.Copy(shadow_.data());
      break;
    case Op::kSwap: {
      uint32_t p[3];
      std::memcpy(p, payload, sizeof(p));
      target_.Swap(p[0], p[1], p[2], shadow_.data());
      break;
    }
    case Op::kFlush:
      target_.Flush();
      break;
    case Op::kWhenDone: {
      std::function<void()>* held;
      std::memcpy(&held, payload, sizeof(held));
      target_.WhenDone(std::move(*held));
      delete held;
      break;
    }
    case Op::kInvalidate: {
      uint32_t p[2];
      std::memcpy(p, payload, sizeof(p));
      target_.Invalidate(p[0], p[1]);
      break;
    }
    case Op::kGammaRamp: {
      const uint32_t* words = reinterpret_cast<const uint32_t*>(payload);
      target_.GammaRamp(words, words + 256);
      break;
    }
  }
}

}  // namespace any360::gfx
