#include "any360/sys/ldi.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace any360::sys {

namespace {

// Block types.
constexpr uint32_t kVerbatim = 1, kAligned = 2, kUncompressed = 3;
constexpr uint32_t kNumChars = 256, kMinMatch = 2, kPrimaryLengths = 7, kSecondaryLengths = 249;
// Position slots of each window size (2^15 to 2^21).
constexpr uint32_t kPositionSlots[7] = {30, 32, 34, 36, 38, 42, 50};
constexpr uint32_t kMaxMainSymbols = kNumChars + 50 * 8;
// A run of the code-length coding may write up to 50 entries past the last; they are kept, as the format's reference
// decoders keep them (the main tree's second part starts from what the first part's last run left).
constexpr uint32_t kLengthSafety = 64;

struct SlotTables {
  uint32_t base[50];
  uint8_t extra[50];
  constexpr SlotTables() : base(), extra() {
    for (uint32_t i = 0; i < 50; ++i) extra[i] = uint8_t(i < 4 ? 0 : i < 36 ? i / 2 - 1 : 17);
    for (uint32_t i = 1; i < 50; ++i) base[i] = base[i - 1] + (1u << extra[i - 1]);
  }
};
constexpr SlotTables kSlots;

}  // namespace

// A canonical Huffman code (lengths up to 16 bits): codes of up to `table_bits` bits are looked up in one step
// (symbol << 5 | length); longer ones are found by length.
struct LzxDecoder::Tree {
  static constexpr uint16_t kLong = 0xFFFF;
  uint32_t symbols = 0, table_bits = 0;
  bool empty = false;
  std::vector<uint8_t> lengths;  // symbols + kLengthSafety
  std::vector<uint16_t> table;   // 2^table_bits
  uint16_t first[18] = {}, count[17] = {}, offset[17] = {};
  std::vector<uint16_t> sorted;  // the symbols by length, then value

  Tree(uint32_t symbol_count, uint32_t bits)
      : symbols(symbol_count), table_bits(bits), lengths(symbol_count + kLengthSafety), table(size_t(1) << bits),
        sorted(symbol_count) {}

  // false for a code that is over-subscribed or incomplete; a code with no symbol is `empty` (true only when
  // `may_be_empty`).
  bool Build(bool may_be_empty) {
    std::fill(std::begin(count), std::end(count), uint16_t(0));
    for (uint32_t s = 0; s < symbols; ++s) ++count[lengths[s]];
    empty = count[0] == symbols;
    if (empty) return may_be_empty;
    int32_t left = 1;
    uint32_t code = 0, index = 0;
    for (uint32_t length = 1; length <= 16; ++length) {
      left = (left << 1) - count[length];
      if (left < 0) return false;
      first[length] = uint16_t(code);
      offset[length] = uint16_t(index);
      code = (code + count[length]) << 1;
      index += count[length];
    }
    if (left) return false;
    // The symbols in code order.
    uint16_t next[17];
    std::copy(std::begin(offset), std::end(offset), next);
    for (uint32_t s = 0; s < symbols; ++s) {
      if (lengths[s]) sorted[next[lengths[s]]++] = uint16_t(s);
    }
    std::fill(table.begin(), table.end(), kLong);
    for (uint32_t length = 1; length <= table_bits; ++length) {
      for (uint32_t i = 0; i < count[length]; ++i) {
        const uint32_t c = first[length] + i, span = 1u << (table_bits - length);
        const uint16_t entry = uint16_t(sorted[offset[length] + i] << 5 | length);
        std::fill_n(table.begin() + (size_t(c) << (table_bits - length)), span, entry);
      }
    }
    return true;
  }
};

// LZX's bitstream: 16-bit little-endian words read most significant bit first. Past the chunk's end it reads zeros;
// `Overrun` says when more than the chunk was used (one zero word of slack, as the reference decoders allow).
struct LzxDecoder::Reader {
  const uint8_t* data;
  size_t size, pos = 0;
  uint64_t buffer = 0;
  int bits = 0;

  void Fill() {
    while (bits <= 48) {
      uint32_t word = 0;
      if (pos + 1 < size) {
        word = uint32_t(data[pos]) | uint32_t(data[pos + 1]) << 8;
      } else if (pos < size) {
        word = data[pos];
      }
      pos += 2;
      buffer |= uint64_t(word) << (48 - bits);
      bits += 16;
    }
  }
  uint32_t Peek(int n) const { return uint32_t(buffer >> (64 - n)); }
  void Remove(int n) {
    buffer <<= n;
    bits -= n;
  }
  uint32_t Read(int n) {
    if (!n) return 0;
    if (bits < n) Fill();
    const uint32_t value = Peek(n);
    Remove(n);
    return value;
  }
  // Bytes used so far.
  size_t Used() const { return pos - size_t(bits) / 8; }
  bool Overrun() const { return Used() > size + 2; }
  // Past the word being read, a whole word more when none is begun (an uncompressed block's header), then bytes.
  // Words are counted from where the reading started, which a padding byte may have put on an odd byte.
  void AlignForBytes() {
    pos -= 2 * (size_t(bits) / 16);
    if (bits % 16 == 0) pos += 2;
    buffer = 0;
    bits = 0;
  }
  int Symbol(const Tree& tree) {
    if (bits < 16) Fill();
    const uint16_t entry = tree.table[Peek(int(tree.table_bits))];
    if (entry != Tree::kLong) {
      Remove(entry & 31);
      return entry >> 5;
    }
    const uint32_t code = Peek(16);
    for (uint32_t length = tree.table_bits + 1; length <= 16; ++length) {
      const uint32_t prefix = code >> (16 - length);
      if (prefix - tree.first[length] < tree.count[length]) {
        Remove(int(length));
        return tree.sorted[tree.offset[length] + prefix - tree.first[length]];
      }
    }
    return -1;
  }
};

LzxDecoder::LzxDecoder()
    : main_(std::make_unique<Tree>(kMaxMainSymbols, 12)),
      length_(std::make_unique<Tree>(kSecondaryLengths, 12)),
      aligned_(std::make_unique<Tree>(8, 7)),
      pretree_(std::make_unique<Tree>(20, 6)) {}

LzxDecoder::~LzxDecoder() = default;

bool LzxDecoder::Init(uint32_t window_bits) {
  if (window_bits < kMinWindowBits || window_bits > kMaxWindowBits) return false;
  window_bits_ = window_bits;
  window_size_ = 1u << window_bits;
  if (window_.size() < window_size_) window_.resize(window_size_);
  main_symbols_ = kNumChars + kPositionSlots[window_bits - kMinWindowBits] * 8;
  main_->symbols = main_symbols_;
  Reset();
  return true;
}

void LzxDecoder::Reset() {
  window_posn_ = frame_posn_ = frame_ = 0;
  output_ = 0;
  r0_ = r1_ = r2_ = 1;
  block_type_ = block_length_ = block_remaining_ = 0;
  header_read_ = intel_started_ = pad_pending_ = false;
  intel_filesize_ = intel_curpos_ = 0;
  // The trees' lengths start at 0: each block codes them as changes.
  std::fill_n(main_->lengths.begin(), main_dirty_, uint8_t(0));
  std::fill_n(length_->lengths.begin(), length_dirty_, uint8_t(0));
  main_dirty_ = length_dirty_ = 0;
}

bool LzxDecoder::ReadLengths(Reader& in, uint8_t* lengths, uint32_t first, uint32_t last) {
  Tree& pre = *pretree_;
  for (uint32_t i = 0; i < 20; ++i) pre.lengths[i] = uint8_t(in.Read(4));
  if (!pre.Build(false)) return false;
  const uint32_t limit = last + kLengthSafety;
  for (uint32_t x = first; x < last;) {
    const int code = in.Symbol(pre);
    if (code < 0) return false;
    uint32_t run = 1;
    uint8_t value = 0;
    if (code == 17) {
      run = in.Read(4) + 4;
    } else if (code == 18) {
      run = in.Read(5) + 20;
    } else if (code == 19) {
      run = in.Read(1) + 4;
      const int delta = in.Symbol(pre);
      if (delta < 0 || delta > 16) return false;
      value = uint8_t((lengths[x] + 17 - delta) % 17);
    } else {
      value = uint8_t((lengths[x] + 17 - code) % 17);
    }
    if (x + run > limit) return false;
    std::fill_n(lengths + x, run, value);
    x += run;
  }
  return !in.Overrun();
}

bool LzxDecoder::ReadBlockHeader(Reader& in) {
  // The padding byte of an uncompressed block of odd length that ended with its chunk.
  if (pad_pending_) {
    ++in.pos;
    pad_pending_ = false;
  }
  block_type_ = in.Read(3);
  const uint32_t high = in.Read(16);
  block_remaining_ = block_length_ = high << 8 | in.Read(8);
  switch (block_type_) {
    case kAligned:
      for (uint32_t i = 0; i < 8; ++i) aligned_->lengths[i] = uint8_t(in.Read(3));
      if (!aligned_->Build(false)) return false;
      [[fallthrough]];
    case kVerbatim:
      // Every length a run may reach (up to kLengthSafety past the last), counted before reading: a corrupt block
      // writes some too.
      main_dirty_ = std::max(main_dirty_, main_symbols_ + kLengthSafety);
      length_dirty_ = kSecondaryLengths + kLengthSafety;
      if (!ReadLengths(in, main_->lengths.data(), 0, kNumChars) ||
          !ReadLengths(in, main_->lengths.data(), kNumChars, main_symbols_) || !main_->Build(false)) {
        return false;
      }
      if (main_->lengths[0xE8]) intel_started_ = true;
      return ReadLengths(in, length_->lengths.data(), 0, kSecondaryLengths) && length_->Build(true);
    case kUncompressed: {
      intel_started_ = true;
      in.AlignForBytes();
      if (in.pos + 12 > in.size) return false;
      auto load = [&](size_t at) {
        return uint32_t(in.data[at]) | uint32_t(in.data[at + 1]) << 8 | uint32_t(in.data[at + 2]) << 16 |
               uint32_t(in.data[at + 3]) << 24;
      };
      r0_ = load(in.pos);
      r1_ = load(in.pos + 4);
      r2_ = load(in.pos + 8);
      in.pos += 12;
      return true;
    }
    default:
      return false;
  }
}

template <bool kAlignedBlock>
bool LzxDecoder::DecodeRun(Reader& in, uint32_t run) {
  uint8_t* const window = window_.data();
  uint32_t posn = window_posn_;
  const uint32_t end = posn + run;
  uint32_t r0 = r0_, r1 = r1_, r2 = r2_;
  const Tree& main = *main_;
  while (posn < end) {
    const int element = in.Symbol(main);
    if (element < 0) return false;
    if (element < int(kNumChars)) {
      window[posn++] = uint8_t(element);
      continue;
    }
    const uint32_t match = uint32_t(element) - kNumChars;
    uint32_t length = match & kPrimaryLengths;
    if (length == kPrimaryLengths) {
      if (length_->empty) return false;
      const int footer = in.Symbol(*length_);
      if (footer < 0) return false;
      length += uint32_t(footer);
    }
    length += kMinMatch;
    const uint32_t slot = match >> 3;
    uint32_t offset;
    if (slot == 0) {
      offset = r0;
    } else if (slot == 1) {
      offset = r1;
      r1 = r0;
      r0 = offset;
    } else if (slot == 2) {
      offset = r2;
      r2 = r0;
      r0 = offset;
    } else {
      const uint32_t extra = kSlots.extra[slot];
      offset = kSlots.base[slot] - 2;
      if (kAlignedBlock && extra >= 3) {
        offset += in.Read(int(extra - 3)) << 3;
        const int aligned = in.Symbol(*aligned_);
        if (aligned < 0) return false;
        offset += uint32_t(aligned);
      } else {
        offset += in.Read(int(extra));
      }
      r2 = r1;
      r1 = r0;
      r0 = offset;
    }
    // A match never crosses its block's run (a frame's end).
    if (length > end - posn) return false;
    uint8_t* to = window + posn;
    if (offset <= posn) {
      const uint8_t* from = to - offset;
      uint32_t i = 0;
      if (offset >= 8) {
        // Eight bytes at a time while they fit in the match (each read is at least eight bytes behind the write, so
        // already written), then byte by byte: the bytes past the match are the previous lap of the window, which a
        // later match may still reference.
        for (; i + 8 <= length; i += 8) {
          uint64_t chunk;
          std::memcpy(&chunk, from + i, 8);
          std::memcpy(to + i, &chunk, 8);
        }
      }
      for (; i < length; ++i) to[i] = from[i];
    } else {
      // From the window's end: only once the stream has gone round it.
      if (offset > output_ + (posn - frame_posn_) || offset > window_size_) return false;
      const uint8_t* from = window + window_size_ - (offset - posn);
      const uint32_t tail = std::min(length, offset - posn);
      for (uint32_t i = 0; i < tail; ++i) to[i] = from[i];
      for (uint32_t i = tail; i < length; ++i) to[i] = window[i - tail];
    }
    posn += length;
  }
  window_posn_ = posn;
  r0_ = r0, r1_ = r1, r2_ = r2;
  return true;
}

bool LzxDecoder::Decompress(const uint8_t* source, size_t size, uint8_t* destination, uint32_t frame) {
  if (!window_size_ || !frame || frame > kFrameSize || frame_posn_ + frame > window_size_) return false;
  Reader in{source, size};
  if (!header_read_) {
    if (in.Read(1)) {
      const uint32_t high = in.Read(16);
      intel_filesize_ = int32_t(high << 16 | in.Read(16));
    }
    header_read_ = true;
  }
  const uint32_t end = frame_posn_ + frame;
  while (window_posn_ < end) {
    if (!block_remaining_ && !ReadBlockHeader(in)) return false;
    const uint32_t run = std::min(block_remaining_, end - window_posn_);
    switch (block_type_) {
      case kVerbatim:
        if (!DecodeRun<false>(in, run)) return false;
        break;
      case kAligned:
        if (!DecodeRun<true>(in, run)) return false;
        break;
      case kUncompressed:
        if (in.pos + run > in.size) return false;
        std::memcpy(window_.data() + window_posn_, source + in.pos, run);
        in.pos += run;
        window_posn_ += run;
        // An odd block is followed by one byte of padding: in this chunk when it holds one more byte (a block that
        // ends with a frame keeps its padding in that frame's chunk), else at the next chunk's start.
        if (run == block_remaining_ && (block_length_ & 1)) {
          if (in.pos < in.size) ++in.pos;
          else pad_pending_ = true;
        }
        break;
      default:
        return false;
    }
    block_remaining_ -= run;
    if (in.Overrun()) return false;
  }

  std::memcpy(destination, window_.data() + frame_posn_, frame);
  // The E8 (x86 CALL) translation: absolute targets back to relative, in the first 32768 frames.
  if (intel_started_ && intel_filesize_ && frame_ <= 32768 && frame > 10) {
    int32_t curpos = intel_curpos_;
    const int32_t filesize = intel_filesize_;
    uint8_t* data = destination;
    uint8_t* const data_end = destination + frame - 10;
    while (data < data_end) {
      if (*data++ != 0xE8) {
        ++curpos;
        continue;
      }
      const int32_t absolute = int32_t(uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 |
                                       uint32_t(data[3]) << 24);
      if (absolute >= -curpos && absolute < filesize) {
        const int32_t relative = absolute >= 0 ? absolute - curpos : absolute + filesize;
        data[0] = uint8_t(relative);
        data[1] = uint8_t(relative >> 8);
        data[2] = uint8_t(relative >> 16);
        data[3] = uint8_t(relative >> 24);
      }
      data += 4;
      curpos += 5;
    }
  }
  if (intel_filesize_) intel_curpos_ += int32_t(frame);

  output_ += frame;
  frame_posn_ += frame;
  ++frame_;
  if (window_posn_ == window_size_) window_posn_ = 0;
  if (frame_posn_ == window_size_) frame_posn_ = 0;
  return true;
}

// --- the LDI contexts ---

LdiContexts& Ldi() {
  static LdiContexts contexts;
  return contexts;
}

LdiContexts::Context* LdiContexts::Find(Guest g, uint32_t handle) {
  for (Context& context : contexts_) {
    if (context.handle == handle) return g.Load32(handle) == kMagic ? &context : nullptr;
  }
  return nullptr;
}

uint32_t LdiContexts::Create(Guest g, uint32_t data_block_max_ptr, uint32_t config, uint32_t alloc, uint32_t free,
                             uint32_t scratch, uint32_t source_min_ptr, uint32_t handle_ptr, GuestCall call,
                             void* user) {
  if (!data_block_max_ptr || !config) return kBadParameters;
  const uint32_t data_block_max = g.Load32(data_block_max_ptr);
  if (source_min_ptr) g.Store32(source_min_ptr, data_block_max + kSourceGrowth);
  if (!handle_ptr) return kNoError;
  g.Store32(handle_ptr, 0);

  const uint32_t window_size = g.Load32(config);
  const uint32_t window_bits = window_size ? uint32_t(std::countr_zero(window_size)) : 0;
  if (!window_size || (window_size & (window_size - 1)) || window_bits < 12 || window_bits > 22) {
    return kNotEnoughMemory;
  }

  Context context;
  context.window_size = window_size;
  context.data_block_max = data_block_max;
  uint32_t address = scratch;
  if (alloc) {
    context.free = free;
    context.context_alloc = call(alloc, kContextSize, user);
    if (context.context_alloc) context.window_alloc = call(alloc, window_size + kWindowExtra, user);
    if (!context.window_alloc) {
      if (free && context.context_alloc) call(free, context.context_alloc, user);
      return kNotEnoughMemory;
    }
    address = context.context_alloc;
  }
  if (!address) return kNotEnoughMemory;

  {
    std::lock_guard lock(mutex_);
    if (window_bits >= LzxDecoder::kMinWindowBits && window_bits <= LzxDecoder::kMaxWindowBits) {
      // A kept decoder whose window is large enough, else a new one.
      auto best = spare_.end();
      for (auto it = spare_.begin(); it != spare_.end(); ++it) {
        if ((*it)->window_bits() >= window_bits && (best == spare_.end() || (*it)->window_bits() < (*best)->window_bits())) {
          best = it;
        }
      }
      if (best != spare_.end()) {
        context.decoder = std::move(*best);
        spare_.erase(best);
      } else {
        context.decoder = std::make_unique<LzxDecoder>();
      }
      context.decoder->Init(window_bits);
    }
    g.Store32(address, kMagic);
    context.handle = address;
    // A context at the same address (one the title forgot to destroy) is replaced.
    if (Context* old = Find(g, address)) {
      if (old->decoder) spare_.push_back(std::move(old->decoder));
      *old = std::move(context);
    } else {
      contexts_.push_back(std::move(context));
    }
  }
  g.Store32(handle_ptr, address);
  return kNoError;
}

uint32_t LdiContexts::Decompress(Guest g, uint32_t handle, uint32_t source, uint32_t source_size,
                                 uint32_t destination, uint32_t size_ptr) {
  std::lock_guard lock(mutex_);
  Context* context = Find(g, handle);
  if (!context || !size_ptr) return kBadParameters;
  const uint32_t size = g.Load32(size_ptr);
  if (!size || size > context->data_block_max) return kBufferOverflow;
  g.Store32(size_ptr, 0);
  if (!context->decoder || size > LzxDecoder::kFrameSize) return kFailed;
  if ((source_size && source + source_size - 1 < source) || destination + size - 1 < destination) return kFailed;
  if (!context->decoder->Decompress(g.host(source), source_size, g.host(destination), size)) return kFailed;
  g.Store32(size_ptr, size);
  return kNoError;
}

uint32_t LdiContexts::ResetDecompression(Guest g, uint32_t handle) {
  std::lock_guard lock(mutex_);
  Context* context = Find(g, handle);
  if (!context) return kBadParameters;
  if (context->decoder) context->decoder->Reset();
  return kNoError;
}

uint32_t LdiContexts::Destroy(Guest g, uint32_t handle, GuestCall call, void* user) {
  uint32_t free = 0, context_alloc = 0, window_alloc = 0;
  {
    std::lock_guard lock(mutex_);
    Context* context = Find(g, handle);
    if (!context) return kBadParameters;
    free = context->free, context_alloc = context->context_alloc, window_alloc = context->window_alloc;
    if (context->decoder) spare_.push_back(std::move(context->decoder));
    if (context != &contexts_.back()) *context = std::move(contexts_.back());
    contexts_.pop_back();
    g.Store32(handle, 0);
  }
  // The title's freer runs outside the lock: the window first, then the context.
  if (free) {
    if (window_alloc) call(free, window_alloc, user);
    if (context_alloc) call(free, context_alloc, user);
  }
  return kNoError;
}

void LdiContexts::Reset() {
  std::lock_guard lock(mutex_);
  for (Context& context : contexts_) {
    if (context.decoder) spare_.push_back(std::move(context.decoder));
  }
  contexts_.clear();
}

size_t LdiContexts::size() const {
  std::lock_guard lock(mutex_);
  return contexts_.size();
}

}  // namespace any360::sys
