#include "any360/gfx/command_processor.h"

#include "any360/gfx/capture.h"

#include <algorithm>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>

#include "any360/sys/objects.h"

namespace any360::gfx {

namespace {
constexpr int kMaxIndirectDepth = 8;
// D3D writes this to a query's ZPass (or, in older runtimes, ZFail) pair when the query ends.
constexpr uint32_t kQueryFinished = 0xFFFFFEED;
// What an ended occlusion query reports: something was drawn.
constexpr uint32_t kFakeSampleCount = 1000;

uint64_t Fnv1a(const std::vector<uint32_t>& words) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (uint32_t w : words) {
    for (int i = 0; i < 4; ++i) hash = (hash ^ ((w >> (8 * i)) & 0xFF)) * 0x100000001B3ull;
  }
  return hash;
}

bool Compare(uint32_t function, uint32_t value, uint32_t reference) {
  switch (function & 7) {
    case 0: return false;  // never
    case 1: return value < reference;
    case 2: return value <= reference;
    case 3: return value == reference;
    case 4: return value != reference;
    case 5: return value >= reference;
    case 6: return value > reference;
    default: return true;  // always
  }
}
}  // namespace

uint32_t GpuSwap(uint32_t value, Endian endian) {
  switch (endian) {
    case Endian::k8in16: return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
    case Endian::k8in32: return std::byteswap(value);
    case Endian::k16in32: return (value >> 16) | (value << 16);
    default: return value;
  }
}

CommandProcessor::CommandProcessor(uint8_t* physical) : physical_(physical), registers_(reg::kRegisterCount, 0) {
  // The gamma ramps' defaults (linear), as the reference starts with them.
  for (uint32_t i = 0; i < 256; ++i) {
    const uint32_t v = i * 0x3FF / 0xFF;
    gamma_table_[i] = v | v << 10 | v << 20;
  }
  for (uint32_t i = 0; i < 128; ++i) {
    const uint32_t entry = ((i * 0xFFFF / 0x7F) & ~0x3Fu) | (i < 0x7F ? 0x200u : 0u) << 16;
    for (uint32_t j = 0; j < 3; ++j) gamma_pwl_[i * 3 + j] = entry;
  }
}

CommandProcessor::~CommandProcessor() { Stop(); }

uint32_t CommandProcessor::LoadRaw(uint32_t address) const {
  return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(Host(address & ~3u))).load(std::memory_order_acquire);
}
void CommandProcessor::StoreRaw(uint32_t address, uint32_t value) {
  std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(Host(address & ~3u))).store(value, std::memory_order_release);
}
uint32_t CommandProcessor::LoadBig(uint32_t address) const { return std::byteswap(LoadRaw(address)); }
void CommandProcessor::StoreBig(uint32_t address, uint32_t value) { StoreRaw(address, std::byteswap(value)); }

uint32_t CommandProcessor::reg(uint32_t index) const {
  if (index >= reg::kRegisterCount) return 0;
  return std::atomic_ref<const uint32_t>(registers_[index]).load(std::memory_order_acquire);
}

void CommandProcessor::InitializeRingBuffer(sys::Guest, uint32_t base, uint32_t size_log2) {
  // RB_BUFSZ: log2 of the size in quadwords.
  ring_base_ = base & 0x1FFFFFFF;
  ring_dwords_ = (1u << (size_log2 + 3)) / 4;
  read_index_ = 0;
  write_index_ = 0;
  SetRegister(reg::CP_RB_BASE, ring_base_);
  SetRegister(reg::CP_RB_CNTL, size_log2);
}

void CommandProcessor::EnableReadPointerWriteBack(sys::Guest, uint32_t address, uint32_t block_log2) {
  write_back_ = address & 0x1FFFFFFF;
  write_back_block_log2_ = block_log2;
  SetRegister(reg::CP_RB_RPTR_ADDR, write_back_);
}

void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  if (index >= reg::kRegisterCount) return;
  std::atomic_ref<uint32_t>(registers_[index]).store(value, std::memory_order_release);
  if (index >= reg::DC_LUT_RW_INDEX && index <= reg::DC_LUT_30_COLOR) GammaRampWrite(index);
  if (index == reg::CP_RB_WPTR) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      write_index_ = ring_dwords_ ? value % ring_dwords_ : value;
    }
    kicked_.notify_all();
  }
}

uint32_t CommandProcessor::ReadRegister(uint32_t index) {
  switch (index) {
    case reg::RB_EDRAM_TIMING: return 0x08100748;
    case reg::RB_BC_CONTROL: return 0x0000200E;
    case reg::D1MODE_V_COUNTER: return display_ & 0xFFFF;  // in the blank: the last visible line
    case reg::D1MODE_INTERRUPT_STATUS: return 1;            // a vertical blank
    case reg::D1MODE_VIEWPORT_SIZE: return display_;
    case reg::CP_RB_RPTR: return read_index_;
    case reg::CP_RB_WPTR: return write_index_;
    default: return reg(index);
  }
}

void CommandProcessor::Vblank() { ++counter_; }

void CommandProcessor::Interrupt(uint32_t source, uint32_t cpu) {
  if (interrupt_) {
    interrupt_(source, cpu);
  } else {
    sys::Graphics().RaiseInterrupt(sys::Objects().guest(), source);
  }
}

void CommandProcessor::SetRegister(uint32_t index, uint32_t value) {
  if (index >= reg::kRegisterCount) return;
  std::atomic_ref<uint32_t>(registers_[index]).store(value, std::memory_order_release);
  if (index >= reg::DC_LUT_RW_INDEX && index <= reg::DC_LUT_30_COLOR) {
    GammaRampWrite(index);
    return;
  }
  if (index >= reg::SCRATCH_REG0 && index < reg::SCRATCH_REG0 + 8) {
    // A scratch register the CPU watches: mirrored to memory when its bit of SCRATCH_UMSK is set.
    const uint32_t n = index - reg::SCRATCH_REG0;
    if (reg(reg::SCRATCH_UMSK) & (1u << n)) {
      if (renderer_) renderer_->Flush();
      StoreBig(reg(reg::SCRATCH_ADDR) + 4 * n, value);
    }
  } else if (index == reg::COHER_STATUS_HOST) {
    // A coherency request: pending (bit 31) until a WAIT_REG_MEM on this register completes it.
    std::atomic_ref<uint32_t>(registers_[index]).fetch_or(0x80000000u, std::memory_order_acq_rel);
  }
}

void CommandProcessor::GammaRampWrite(uint32_t index) {
  // As the reference keeps the ramps: DC_LUT_RW_INDEX selects an entry (and restarts at red); DC_LUT_SEQ_COLOR and
  // DC_LUT_PWL_DATA take red, green, blue in turn, then move to the next entry; DC_LUT_30_COLOR takes a whole entry.
  // DC_LUT_WRITE_EN_MASK enables blue, green, red (bits 0-2).
  const uint32_t rw_index = reg(reg::DC_LUT_RW_INDEX), enable = reg(reg::DC_LUT_WRITE_EN_MASK) & 7;
  auto next_index = [&](uint32_t value) {
    std::atomic_ref<uint32_t>(registers_[reg::DC_LUT_RW_INDEX]).store(value, std::memory_order_release);
    gamma_component_ = 0;
  };
  switch (index) {
    case reg::DC_LUT_RW_INDEX:
      gamma_component_ = 0;
      break;
    case reg::DC_LUT_SEQ_COLOR: {
      if (enable & (1u << (2 - gamma_component_))) {
        const uint32_t shift = 10 * (2 - gamma_component_), value = ((reg(reg::DC_LUT_SEQ_COLOR) & 0xFFFF) >> 6) & 0x3FF;
        uint32_t& entry = gamma_table_[rw_index & 0xFF];
        entry = (entry & ~(0x3FFu << shift)) | value << shift;
      }
      if (++gamma_component_ >= 3) next_index((rw_index & ~0xFFu) | ((rw_index + 1) & 0xFF));
      break;
    }
    case reg::DC_LUT_PWL_DATA: {
      if (enable & (1u << (2 - gamma_component_))) {
        gamma_pwl_[(rw_index & 0x7F) * 3 + gamma_component_] = reg(reg::DC_LUT_PWL_DATA) & 0xFFC0FFC0u;
      }
      if (++gamma_component_ >= 3) next_index((rw_index & ~0x7Fu) | ((rw_index + 1) & 0x7F));
      break;
    }
    case reg::DC_LUT_30_COLOR: {
      uint32_t& entry = gamma_table_[rw_index & 0xFF];
      const uint32_t value = reg(reg::DC_LUT_30_COLOR);
      for (uint32_t c = 0; c < 3; ++c) {
        if (enable & (1u << c)) entry = (entry & ~(0x3FFu << (10 * c))) | (value & (0x3FFu << (10 * c)));
      }
      next_index((rw_index & ~0xFFu) | ((rw_index + 1) & 0xFF));
      break;
    }
    default:
      break;
  }
}

void CommandProcessor::RestoreGammaRamp(const uint32_t* table, const uint32_t* pwl) {
  std::copy(table, table + 256, gamma_table_.begin());
  std::copy(pwl, pwl + 128 * 3, gamma_pwl_.begin());
}

void CommandProcessor::MakeCoherent() {
  const uint32_t status = reg(reg::COHER_STATUS_HOST);
  if (!(status & 0x80000000)) return;
  if (renderer_) renderer_->Invalidate(reg(reg::COHER_BASE_HOST), reg(reg::COHER_SIZE_HOST));
  std::atomic_ref<uint32_t>(registers_[reg::COHER_STATUS_HOST]).store(0, std::memory_order_release);
}

bool CommandProcessor::Test(uint32_t wait_info, uint32_t poll, uint32_t reference, uint32_t mask, bool coherent) {
  uint32_t value;
  if (wait_info & 0x10) {
    value = GpuSwap(LoadRaw(poll & ~3u), Endian(poll & 3));
  } else {
    // The register file itself, not the MMIO view.
    if (coherent && poll == reg::COHER_STATUS_HOST) MakeCoherent();
    value = reg(poll);
  }
  return Compare(wait_info, value & mask, reference);
}

bool CommandProcessor::Wait(uint32_t wait_info, uint32_t poll, uint32_t reference, uint32_t mask, uint32_t interval) {
  while (!Test(wait_info, poll, reference, mask, true)) {
    if (stopping_) return false;
    if (replaying_) {
      ++replay_skipped_waits_;
      return true;
    }
    // The interval is in units of 1/256 ms: long waits sleep, short ones yield.
    if (interval >= 0x100) {
      std::this_thread::sleep_for(std::chrono::microseconds(interval * 1000 / 0x100));
    } else {
      std::this_thread::yield();
    }
  }
  return true;
}

uint32_t CommandProcessor::Next(Reader& r) const {
  const uint32_t value = LoadBig(r.base + 4 * (r.offset % r.capacity));
  ++r.offset;
  --r.remaining;
  return value;
}

bool CommandProcessor::Execute() {
  const uint32_t write = write_index_;
  const uint32_t read = read_index_;
  if (!ring_dwords_ || write == read) return true;
  busy_ = true;
  if (capture_) capture_->Kick(ring_base_, ring_dwords_, read, write);
  Reader r{ring_base_, ring_dwords_, read, (write + ring_dwords_ - read) % ring_dwords_};
  bool ok = true;
  while (r.remaining && ok) {
    ok = ExecutePacket(r, 0);
    if (stopping_) break;
  }
  const bool stopped = stopping_;
  if (!stopped) {
    read_index_ = write;
    SetRegister(reg::CP_RB_RPTR, write);
    if (write_back_) {
      if (renderer_) renderer_->Flush();  // the CPU may reuse what the read pointer passed
      StoreBig(write_back_, write);
    }
  }
  busy_ = false;
  return !stopped;
}

void CommandProcessor::ExecuteIndirect(uint32_t address, uint32_t dwords) {
  if (!dwords) return;
  Reader r{address & 0x1FFFFFFF, dwords, 0, dwords};
  while (r.remaining && ExecutePacket(r, 1) && !stopping_) {
  }
}

bool CommandProcessor::ExecutePacket(Reader& r, int depth) {
  const uint32_t packet = Next(r);
  if (packet == 0) return true;  // a zero dword is padding, not a type-0 write of register 0
  switch (packet >> 30) {
    case 0: {
      // Consecutive registers from `first`, or every value to the one register.
      const uint32_t count = ((packet >> 16) & 0x3FFF) + 1, first = packet & 0x7FFF;
      const bool one_register = packet & 0x8000;
      if (count > r.remaining) return false;
      for (uint32_t i = 0; i < count; ++i) SetRegister(one_register ? first : first + i, Next(r));
      return true;
    }
    case 1: {
      if (r.remaining < 2) return false;  // the reference reads past the end; this stops instead
      const uint32_t first = Next(r), second = Next(r);
      SetRegister(packet & 0x7FF, first);
      SetRegister((packet >> 11) & 0x7FF, second);
      return true;
    }
    case 2:
      return true;
    default:
      return ExecuteType3(r, packet, depth);
  }
}

bool CommandProcessor::ExecuteType3(Reader& r, uint32_t packet, int depth) {
  const uint32_t opcode = (packet >> 8) & 0x7F, count = ((packet >> 16) & 0x3FFF) + 1;
  if (count > r.remaining) return false;
  const uint32_t end = r.offset + count;
  auto skip_rest = [&] {
    if (r.offset < end) {
      r.remaining -= end - r.offset;
      r.offset = end;
    }
  };
  // Bin (tile) predication: a predicated packet runs only for the selected bins; a predicated swap never runs.
  if ((packet & 1) && (!(bin_select_ & bin_mask_) || opcode == pm4::XE_SWAP)) {
    skip_rest();
    return true;
  }
  ++opcode_counts_[opcode];
  auto has = [&](uint32_t n) { return end - r.offset >= n; };
  auto write_constants = [&](uint32_t type, uint32_t index, uint32_t n, auto value_at) {
    static constexpr uint32_t kBase[] = {reg::kAluConstants, reg::kFetchConstants, reg::kBoolConstants, reg::kLoopConstants,
                                         reg::kContextRegisters};
    if (type > 4) return;
    for (uint32_t i = 0; i < n; ++i) SetRegister(kBase[type] + index + i, value_at(i));
  };

  // What the CPU can observe of the GPU's progress: the draws so far must have run first (the CPU may then reuse
  // the memory they read). Not the occlusion query and screen extent reports: their values are fixed (as the
  // reference's), not results of the draws, and titles issue them around many draws (thousands a frame).
  switch (opcode) {
    case pm4::INTERRUPT: case pm4::REG_TO_MEM: case pm4::MEM_WRITE: case pm4::COND_WRITE: case pm4::EVENT_WRITE_SHD:
      if (renderer_) renderer_->Flush();
      break;
    default:
      break;
  }
  switch (opcode) {
    case pm4::ME_INIT:
    case pm4::NOP:
    case pm4::VIZ_QUERY:
    case pm4::WAIT_FOR_IDLE:
    case pm4::CONTEXT_UPDATE:
    case pm4::INVALIDATE_STATE:
      break;
    case pm4::INTERRUPT:
      if (has(1)) {
        const uint32_t cpus = Next(r);
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) Interrupt(1, cpu);
        }
      }
      break;
    case pm4::XE_SWAP:
      if (has(4)) {
        Next(r);  // 'SWAP'
        const uint32_t frontbuffer = Next(r), width = Next(r), height = Next(r);
        if (renderer_) {
          renderer_->GammaRamp(gamma_table_.data(), gamma_pwl_.data());
          renderer_->Swap(frontbuffer & 0x1FFFFFFF, width, height, registers_.data());
        }
        ++counter_;  // the counter fences read moves with swaps as well as vertical blanks
      }
      break;
    case pm4::INDIRECT_BUFFER:
    case pm4::INDIRECT_BUFFER_PFD:
      if (has(2)) {
        const uint32_t address = Next(r) & 0x1FFFFFFF, dwords = Next(r) & 0xFFFFF;
        if (depth < kMaxIndirectDepth && dwords) {
          Reader ib{address, dwords, 0, dwords};
          while (ib.remaining && ExecutePacket(ib, depth + 1) && !stopping_) {
          }
        }
      }
      break;
    case pm4::WAIT_REG_MEM:
      if (has(5)) {
        const uint32_t wait_info = Next(r), poll = Next(r), reference = Next(r), mask = Next(r), interval = Next(r);
        if (!Wait(wait_info, poll, reference, mask, interval)) return false;
      }
      break;
    case pm4::REG_RMW:
      if (has(3)) {
        const uint32_t info = Next(r), and_mask = Next(r), or_mask = Next(r);
        uint32_t value = reg(info & 0x1FFF);
        value &= (info >> 31) ? reg(and_mask & 0x1FFF) : and_mask;
        value |= ((info >> 30) & 1) ? reg(or_mask & 0x1FFF) : or_mask;
        SetRegister(info & 0x1FFF, value);
      }
      break;
    case pm4::REG_TO_MEM:
      if (has(2)) {
        const uint32_t index = Next(r), address = Next(r);
        StoreRaw(address & ~3u, GpuSwap(reg(index), Endian(address & 3)));
      }
      break;
    case pm4::MEM_WRITE:
      if (has(1)) {
        const uint32_t target = Next(r);
        uint32_t address = target & ~3u;
        while (r.offset < end) {
          StoreRaw(address, GpuSwap(Next(r), Endian(target & 3)));
          address += 4;
        }
      }
      break;
    case pm4::COND_WRITE:
      if (has(6)) {
        const uint32_t wait_info = Next(r), poll = Next(r), reference = Next(r), mask = Next(r), target = Next(r),
                       data = Next(r);
        if (Test(wait_info, poll, reference, mask, false)) {
          if (wait_info & 0x100) {
            StoreRaw(target & ~3u, GpuSwap(data, Endian(target & 3)));
          } else {
            SetRegister(target, data);
          }
        }
      }
      break;
    case pm4::EVENT_WRITE:
      if (has(1)) SetRegister(reg::VGT_EVENT_INITIATOR, Next(r) & 0x3F);
      break;
    case pm4::EVENT_WRITE_SHD:
      if (has(3)) {
        // A fence: the value, or the vertical blank counter, written once the shaders are done.
        const uint32_t initiator = Next(r), address = Next(r), value = Next(r);
        SetRegister(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
        StoreRaw(address & ~3u, GpuSwap((initiator >> 31) ? counter_.load() : value, Endian(address & 3)));
      }
      break;
    case pm4::EVENT_WRITE_EXT:
      if (has(2)) {
        // The screen extents of the previous draws, as big-endian halves: the whole surface.
        const uint32_t initiator = Next(r), address = Next(r) & ~3u;
        SetRegister(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
        static constexpr uint16_t kExtents[] = {0 >> 3, 8192 >> 3, 0 >> 3, 8192 >> 3, 0, 1};
        for (uint32_t i = 0; i < 6; ++i) {
          const uint16_t big = std::byteswap(kExtents[i]);
          std::memcpy(Host(address + 2 * i), &big, 2);
        }
      }
      break;
    case pm4::EVENT_WRITE_ZPD:
      if (has(1)) {
        // An occlusion query: counts at RB_SAMPLE_COUNT_ADDR (Total, ZFail, ZPass, StencilFail; A and B each).
        SetRegister(reg::VGT_EVENT_INITIATOR, Next(r) & 0x3F);
        const uint32_t counts = reg(reg::RB_SAMPLE_COUNT_ADDR) & 0x1FFFFFFF;
        const bool ended = LoadBig(counts + 16) == kQueryFinished || LoadBig(counts + 20) == kQueryFinished ||
                           LoadBig(counts + 8) == kQueryFinished || LoadBig(counts + 12) == kQueryFinished;
        std::memset(Host(counts), 0, 32);
        if (ended) {
          StoreRaw(counts + 0, kFakeSampleCount);   // Total_A
          StoreRaw(counts + 16, kFakeSampleCount);  // ZPass_A
        }
      }
      break;
    case pm4::DRAW_INDX:
    case pm4::DRAW_INDX_2:
      Draw(r, opcode, end);
      break;
    case pm4::SET_CONSTANT:
      if (has(1)) {
        const uint32_t offset_type = Next(r);
        write_constants((offset_type >> 16) & 0xFF, offset_type & 0x7FF, end - r.offset, [&](uint32_t) { return Next(r); });
      }
      break;
    case pm4::SET_CONSTANT2:
    case pm4::SET_SHADER_CONSTANTS:
      if (has(1)) {
        const uint32_t index = Next(r) & 0xFFFF;
        for (uint32_t i = 0; r.offset < end; ++i) SetRegister(index + i, Next(r));
      }
      break;
    case pm4::LOAD_ALU_CONSTANT:
      if (has(3)) {
        const uint32_t address = Next(r) & 0x3FFFFFFF, offset_type = Next(r), dwords = Next(r) & 0xFFF;
        write_constants((offset_type >> 16) & 0xFF, offset_type & 0x7FF, dwords,
                        [&](uint32_t i) { return LoadBig(address + 4 * i); });
      }
      break;
    case pm4::IM_LOAD:
      if (has(2)) {
        const uint32_t address_type = Next(r), start_size = Next(r);
        if ((address_type & 3) > 1) return false;  // neither vertex nor pixel: the buffer stops
        LoadShader(address_type & 3, address_type & ~3u, start_size & 0xFFFF);
      }
      break;
    case pm4::IM_LOAD_IMMEDIATE:
      if (has(2)) {
        const uint32_t type = Next(r), start_size = Next(r);
        const uint32_t dwords = start_size & 0xFFFF;
        if (type > 1) return false;
        // The microcode follows in the packet (the primary ring never wraps inside a shader the runtime writes).
        if (has(dwords)) LoadShader(type, r.base + 4 * (r.offset % r.capacity), dwords);
      }
      break;
    case pm4::SET_BIN_MASK:
      if (has(2)) {
        const uint64_t high = Next(r);
        bin_mask_ = high << 32 | Next(r);
      }
      break;
    case pm4::SET_BIN_SELECT:
      if (has(2)) {
        const uint64_t high = Next(r);
        bin_select_ = high << 32 | Next(r);
      }
      break;
    case pm4::SET_BIN_MASK_LO:
      if (has(1)) bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | Next(r);
      break;
    case pm4::SET_BIN_MASK_HI:
      if (has(1)) bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | uint64_t(Next(r)) << 32;
      break;
    case pm4::SET_BIN_SELECT_LO:
      if (has(1)) bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | Next(r);
      break;
    case pm4::SET_BIN_SELECT_HI:
      if (has(1)) bin_select_ = (bin_select_ & 0xFFFFFFFFull) | uint64_t(Next(r)) << 32;
      break;
    default:
      // An opcode the title's runtime does not emit: skipped, and the rest of the buffer with it.
      skip_rest();
      return false;
  }
  skip_rest();
  return true;
}

void CommandProcessor::Draw(Reader& r, uint32_t opcode, uint32_t end) {
  auto has = [&](uint32_t n) { return end - r.offset >= n; };
  if (opcode == pm4::DRAW_INDX) {
    if (!has(1)) return;
    Next(r);  // the visibility query
  }
  if (!has(1)) return;
  const uint32_t initiator = Next(r);
  SetRegister(reg::VGT_DRAW_INITIATOR, initiator);
  DrawCall draw;
  draw.primitive_type = initiator & 0x3F;
  draw.source = DrawSource((initiator >> 6) & 3);
  draw.major_mode_explicit = ((initiator >> 8) & 3) != 0;
  draw.index_32 = (initiator >> 11) & 1;
  draw.index_count = initiator >> 16;
  if (draw.source == DrawSource::kDma) {
    if (!has(2)) return;
    const uint32_t base = Next(r), size = Next(r);
    SetRegister(reg::VGT_DMA_BASE, base);
    SetRegister(reg::VGT_DMA_SIZE, size);
    draw.index_base = base & 0x1FFFFFFF & ~(draw.index_32 ? 3u : 1u);
    draw.index_endian = Endian(size >> 30);
    draw.index_bytes = (size & 0x00FFFFFF) * (draw.index_32 ? 4 : 2);
  } else if (draw.source == DrawSource::kImmediate) {
    // Indices inside the packet: no known title emits them (the reference does not draw them either).
    while (r.offset < end) draw.immediate.push_back(Next(r));
  } else if (draw.source != DrawSource::kAuto) {
    return;
  }
  // A visibility query that kills the pixels draws nothing.
  const uint32_t viz = reg(reg::PA_SC_VIZ_QUERY);
  if ((viz & 1) && (viz & 0x80)) return;
  // RB_MODECONTROL's EDRAM mode 6 is a copy (a resolve); every other mode is a draw the renderer judges.
  const uint32_t mode = reg(reg::RB_MODECONTROL) & 7;
  if (!renderer_) return;
  if (mode == 6) {
    renderer_->Copy(registers_.data());
  } else {
    draw.edram_mode = mode;
    draw.vertex_shader = &shaders_[0];
    draw.pixel_shader = &shaders_[1];
    draw.registers = registers_.data();
    renderer_->Draw(draw);
  }
}

void CommandProcessor::LoadShader(uint32_t type, uint32_t address, uint32_t dwords) {
  if (type > 1) return;
  Shader& shader = shaders_[type];
  shader.type = type;
  shader.address = address & 0x1FFFFFFF;
  shader.microcode.resize(dwords);
  for (uint32_t i = 0; i < dwords; ++i) shader.microcode[i] = LoadBig(shader.address + 4 * i);
  shader.hash = Fnv1a(shader.microcode);
  if (renderer_) renderer_->ShaderLoaded(shader);
}

void CommandProcessor::Restore(const uint32_t* registers, uint32_t count) {
  for (uint32_t i = 0; i < count && i < reg::kRegisterCount; ++i) {
    std::atomic_ref<uint32_t>(registers_[i]).store(registers[i], std::memory_order_release);
  }
  ring_base_ = reg(reg::CP_RB_BASE) & 0x1FFFFFFF;
  // A pending coherency request was the CPU's: done by now.
  std::atomic_ref<uint32_t>(registers_[reg::COHER_STATUS_HOST]).store(0, std::memory_order_release);
}

void CommandProcessor::RestoreShader(uint32_t type, std::vector<uint32_t> microcode) {
  if (type > 1 || microcode.empty()) return;
  Shader& shader = shaders_[type];
  shader.type = type;
  shader.address = 0;
  shader.microcode = std::move(microcode);
  shader.hash = Fnv1a(shader.microcode);
  if (renderer_) renderer_->ShaderLoaded(shader);
}

bool CommandProcessor::Replay(uint32_t ring_base, uint32_t ring_dwords, uint32_t read, uint32_t write) {
  ring_base_ = ring_base & 0x1FFFFFFF;
  ring_dwords_ = ring_dwords;
  read_index_ = read;
  write_index_ = write;
  write_back_ = 0;  // the CPU is not there to read it
  replaying_ = true;
  const bool ok = Execute();
  replaying_ = false;
  return ok;
}

void CommandProcessor::Start() {
  Stop();
  stopping_ = false;
  worker_ = std::thread([this] {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
      kicked_.wait(lock, [this] { return stopping_ || write_index_ != read_index_; });
      if (stopping_) break;
      lock.unlock();
      Execute();
      lock.lock();
    }
  });
}

void CommandProcessor::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  kicked_.notify_all();
  if (worker_.joinable()) worker_.join();
  stopping_ = false;
}

}  // namespace any360::gfx
