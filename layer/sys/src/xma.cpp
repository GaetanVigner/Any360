// The XMA decoder: see xma.h. Ported from Xenia's XmaContext / XmaDecoder (BSD license, copyright Ben Vanik and the
// Xenia contributors; adapted for ReXGlue by Tom Clay), crediting koolkdev's libertyv xma2dec.
#include "any360/sys/xma.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
}

namespace any360::sys {

namespace {

constexpr uint32_t kBytesPerPacket = 2048, kBitsPerPacket = kBytesPerPacket * 8, kBitsPerPacketHeader = 32,
                   kBitsPerFrameHeader = 15, kBytesPerPacketHeader = 4,
                   kBytesPerPacketData = kBytesPerPacket - kBytesPerPacketHeader, kBytesPerSample = 2,
                   kSamplesPerFrame = 512, kBytesPerFrameChannel = kSamplesPerFrame * kBytesPerSample,
                   kOutputBytesPerBlock = 256, kMaxFrameSizeInBits = 0x4000 - kBitsPerPacketHeader,
                   kMaxFrameLength = 0x7FFF;
// The bitstream declares a start padding the hardware discards and FFmpeg's xmaframes decoder emits as output: its
// sample numbering runs this far ahead of the one the guest's loop offsets use (measured by Xenia).
constexpr uint32_t kDecoderStartPadding = 192;
constexpr int kSampleRates[4] = {24000, 32000, 44100, 48000};

// The packet header: frames starting in the packet (XMA2), first frame's bit offset, metadata, packets to skip.
uint8_t PacketFrameCount(const uint8_t* packet) { return packet[0] >> 2; }
uint32_t PacketFrameOffset(const uint8_t* packet) {
  return uint32_t(uint16_t(((packet[0] & 0x3) << 13) | (packet[1] << 5) | (packet[2] >> 3))) + 32;
}
bool PacketIsXma2(const uint8_t* packet) { return (packet[2] & 0x7) == 1; }
uint8_t PacketSkipCount(const uint8_t* packet) { return packet[3]; }

// Big-endian bits read most significant first.
class BitStream {
 public:
  BitStream(const uint8_t* buffer, size_t size_bits) : buffer_(buffer), size_(size_bits) {}
  void SetOffset(size_t offset) { offset_ = std::min(offset, size_); }
  size_t offset() const { return offset_; }
  size_t Remaining() const { return size_ - offset_; }
  void Advance(size_t bits) { SetOffset(offset_ + bits); }
  uint64_t Peek(size_t bits) const {  // up to 57 bits
    const size_t byte = offset_ >> 3, rel = offset_ & 7;
    uint64_t v = 0;
    std::memcpy(&v, buffer_ + byte, 8);
    v = std::byteswap(v);
    v >>= 64 - (rel + bits);
    return v & ((1ull << bits) - 1);
  }
  uint64_t Read(size_t bits) {
    const uint64_t v = Peek(bits);
    Advance(bits);
    return v;
  }
  // `bits` bits to `out`, keeping the source's bit alignment within the first byte; returns that alignment.
  size_t Copy(uint8_t* out, size_t bits) {
    const size_t rel = offset_ & 7;
    size_t left = bits, at = 0;
    if (rel) {
      const uint64_t head = Peek(8 - rel);
      out[at] = uint8_t((out[at] & ~((1u << rel) - 1)) | head);
      left -= 8 - rel;
      Advance(8 - rel);
      ++at;
    }
    if (left >= 8) {
      std::memcpy(out + at, buffer_ + (offset_ >> 3), left / 8);
      at += left / 8;
      Advance(left / 8 * 8);
      left -= left / 8 * 8;
    }
    if (left) {
      const uint64_t tail = Peek(left) << (8 - left);
      out[at] = uint8_t((out[at] & ((1u << left) - 1)) | tail);
      Advance(left);
    }
    return rel;
  }

 private:
  const uint8_t* buffer_;
  size_t size_, offset_ = 0;
};

// The output ring the title reads, in bytes.
struct Ring {
  uint8_t* buffer;
  uint32_t capacity, read = 0, write = 0;
  bool empty() const { return read == write; }
  uint32_t WriteCount() const {
    if (read == write) return capacity;
    return write < read ? read - write : capacity - write + read;
  }
  void Write(const uint8_t* data, uint32_t count) {
    count = std::min(count, capacity);
    if (!count) return;
    if (write + count < capacity) {
      std::memcpy(buffer + write, data, count);
      write += count;
    } else {
      const uint32_t left = capacity - write;
      std::memcpy(buffer + write, data, left);
      std::memcpy(buffer, data + left, count - left);
      write = count - left;
    }
  }
};

// The 64-byte context, its big-endian words unpacked into bitfields (from the low bit, as the hardware's).
struct ContextData {
  uint32_t input_buffer_0_packet_count : 12;
  uint32_t loop_count : 8;
  uint32_t input_buffer_0_valid : 1;
  uint32_t input_buffer_1_valid : 1;
  uint32_t output_buffer_block_count : 5;
  uint32_t output_buffer_write_offset : 5;
  uint32_t input_buffer_1_packet_count : 12;
  uint32_t loop_subframe_start : 2;
  uint32_t loop_subframe_end : 3;
  uint32_t loop_subframe_skip : 3;
  uint32_t subframe_decode_count : 4;
  uint32_t output_buffer_padding : 3;
  uint32_t sample_rate : 2;
  uint32_t is_stereo : 1;
  uint32_t unknown_1 : 1;
  uint32_t output_buffer_valid : 1;
  uint32_t input_buffer_read_offset : 26;
  uint32_t error_status : 5;
  uint32_t error_set : 1;
  uint32_t loop_start : 26;
  uint32_t parser_error_status : 5;
  uint32_t parser_error_set : 1;
  uint32_t loop_end : 26;
  uint32_t packet_metadata : 5;
  uint32_t current_buffer : 1;
  uint32_t input_buffer_0_ptr;
  uint32_t input_buffer_1_ptr;
  uint32_t output_buffer_ptr;
  uint32_t work_buffer_ptr;
  uint32_t output_buffer_read_offset : 5;
  uint32_t : 25;
  uint32_t stop_when_done : 1;
  uint32_t interrupt_when_done : 1;
  uint32_t reserved[6];

  explicit ContextData(const uint8_t* guest) {
    auto* words = reinterpret_cast<uint32_t*>(this);
    for (uint32_t i = 0; i < 16; ++i) words[i] = LoadWord(guest, i);
  }
  // Acquire per word: a published output position also makes the samples written before it visible.
  static uint32_t LoadWord(const uint8_t* guest, uint32_t index) {
    std::atomic_ref<uint32_t> word(*reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(guest) + 4 * index));
    return std::byteswap(word.load(std::memory_order_acquire));
  }
  // The guest's words are written by the title too: a masked compare-exchange keeps its neighbouring fields.
  static void UpdateWord(uint8_t* guest, uint32_t index, uint32_t mask, uint32_t value) {
    if (!mask) return;
    std::atomic_ref<uint32_t> word(*reinterpret_cast<uint32_t*>(guest + 4 * index));
    uint32_t previous = word.load(std::memory_order_relaxed), next;
    do {
      next = std::byteswap((std::byteswap(previous) & ~mask) | (value & mask));
    } while (!word.compare_exchange_weak(previous, next, std::memory_order_release, std::memory_order_relaxed));
  }
  bool InputValid(uint32_t index) const { return index ? input_buffer_1_valid : input_buffer_0_valid; }
  bool CurrentInputValid() const { return InputValid(current_buffer); }
  bool AnyInputValid() const { return input_buffer_0_valid || input_buffer_1_valid; }
  uint32_t InputAddress(uint32_t index) const { return index ? input_buffer_1_ptr : input_buffer_0_ptr; }
  uint32_t InputPackets(uint32_t index) const { return index ? input_buffer_1_packet_count : input_buffer_0_packet_count; }
  bool ConsumeOnly() const { return (input_buffer_0_packet_count | input_buffer_1_packet_count) == 0; }
};
static_assert(sizeof(ContextData) == 64);

struct PacketInfo {
  uint8_t frame_count = 0, current_frame = 0;
  uint32_t current_frame_size = 0;
  bool LastFrameInPacket() const { return frame_count == 0 || current_frame == frame_count - 1; }
};

}  // namespace

struct XmaDecoder::Context {
  Guest g;
  uint32_t address;
  std::mutex lock;
  std::atomic<bool> enabled{false};
  AVPacket* packet = nullptr;
  const AVCodec* codec = nullptr;
  AVCodecContext* codec_context = nullptr;
  AVFrame* frame = nullptr;
  std::array<uint8_t, kBytesPerPacketData * 2> input{};
  std::array<uint8_t, 1 + 4096> xma_frame{};  // the first byte holds the frame's bit padding
  std::array<uint8_t, kBytesPerFrameChannel * 2> raw{}, decoded{}, carry{};
  int32_t remaining_output_blocks = 0;
  uint8_t remaining_subframes = 0, loop_output_limit = 0, pending_output_limit = 0, pending_start_skip = 0;
  bool loop_start_skip_pending = false, carry_valid = false;

  Context(Guest guest, uint32_t at) : g(guest), address(at) {
    packet = av_packet_alloc();
    codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
    codec_context = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (codec_context) {
      codec_context->channels = 0;
      codec_context->sample_rate = 0;
    }
    frame = av_frame_alloc();
  }
  ~Context() {
    if (codec_context) avcodec_free_context(&codec_context);
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
  }
  uint8_t* Guest32(uint32_t a) const { return g.host(a); }
  // A physical address as the host sees it: the 0xA0000000 view maps all of physical memory.
  uint8_t* Physical(uint32_t p) const { return g.host(0xA0000000u | (p & 0x1FFFFFFFu)); }

  void ResetDecoder() {
    // A new logical stream: reopening the codec (next decode) drops the previous one's overlap-add tail.
    if (codec_context) {
      codec_context->sample_rate = 0;
      codec_context->channels = 0;
    }
    raw.fill(0);
    decoded.fill(0);
    carry.fill(0);
    carry_valid = false;
    pending_output_limit = pending_start_skip = 0;
    remaining_subframes = 0;
    loop_output_limit = 0;
    loop_start_skip_pending = false;
  }

  void Clear() {
    std::lock_guard guard(lock);
    uint8_t* at = Guest32(address);
    ContextData data(at);
    // Input invalid, read offset back to the first frame, output emptied.
    ContextData::UpdateWord(at, 0, (1u << 20) | (1u << 21) | (31u << 27), 0);
    ContextData::UpdateWord(at, 1, 1u << 31, 0);
    ContextData::UpdateWord(at, 2, 0x03FFFFFFu, kBitsPerPacketHeader);
    ContextData::UpdateWord(at, 9, 31u, 0);
    (void)data;
    ResetDecoder();
  }

  static void SwapInput(ContextData* d) {
    if (d->current_buffer == 0) d->input_buffer_0_valid = 0;
    else d->input_buffer_1_valid = 0;
    d->current_buffer ^= 1;
    d->input_buffer_read_offset = kBitsPerPacketHeader;
  }

  void UpdateLoop(ContextData* d) {
    if (d->loop_count == 0) return;
    const uint32_t start = std::max(kBitsPerPacketHeader, uint32_t(d->loop_start));
    const uint32_t end = std::max(kBitsPerPacketHeader, uint32_t(d->loop_end));
    if (d->input_buffer_read_offset != end) return;
    d->input_buffer_read_offset = start;
    loop_start_skip_pending = true;
    if (d->loop_count < 255) d->loop_count--;
  }

  const uint8_t* NextPacket(ContextData* d, uint32_t next_index, uint32_t packet_count) const {
    if (next_index < packet_count) return Physical(d->InputAddress(d->current_buffer)) + next_index * kBytesPerPacket;
    const uint32_t other = d->current_buffer ^ 1;
    if (!d->InputValid(other) || !d->InputAddress(other)) return nullptr;
    return Physical(d->InputAddress(other));
  }

  static uint32_t NextPacketReadOffset(const uint8_t* buffer, uint32_t next_index, uint32_t packet_count) {
    for (; next_index < packet_count; ++next_index) {
      const uint32_t offset = PacketFrameOffset(buffer + next_index * kBytesPerPacket);
      if (offset <= kMaxFrameSizeInBits) return next_index * kBitsPerPacket + offset;
    }
    return kBitsPerPacketHeader;
  }

  static PacketInfo Info(const uint8_t* packet, uint32_t frame_offset) {
    PacketInfo info;
    const uint32_t first = PacketFrameOffset(packet);
    BitStream stream(packet, kBitsPerPacket);
    stream.SetOffset(first);
    if (frame_offset < first) {
      info.current_frame = 0;
      info.current_frame_size = first - frame_offset;
    }
    while (stream.Remaining() >= kBitsPerFrameHeader) {
      const uint64_t size = stream.Peek(kBitsPerFrameHeader);
      if (size == 0 || size == kMaxFrameLength) break;
      if (stream.offset() == frame_offset) {
        info.current_frame = info.frame_count;
        info.current_frame_size = uint32_t(size);
      }
      info.frame_count++;
      if (size > stream.Remaining()) break;
      stream.Advance(size - 1);
      if (stream.Read(1) == 0) break;
    }
    if (PacketIsXma2(packet)) {
      const uint8_t count = PacketFrameCount(packet);
      if (count > info.frame_count) {
        if (info.current_frame_size == 0) info.current_frame = info.frame_count;
        info.frame_count = count;
      }
    }
    return info;
  }

  Ring OutputRing(ContextData* d) {
    Ring ring{Physical(d->output_buffer_ptr), d->output_buffer_block_count * kOutputBytesPerBlock};
    if (!ring.capacity) return ring;
    ring.read = (d->output_buffer_read_offset * kOutputBytesPerBlock) % ring.capacity;
    ring.write = (d->output_buffer_write_offset * kOutputBytesPerBlock) % ring.capacity;
    remaining_output_blocks = int32_t(ring.WriteCount() / kOutputBytesPerBlock);
    return ring;
  }

  // Only the fields the decoder changed go back, so the title's concurrent writes survive.
  static void StoreMerged(const ContextData& d, const ContextData& initial, uint8_t* at) {
    uint32_t mask = 31u << 27, value = uint32_t(d.output_buffer_write_offset) << 27;
    if (d.loop_count != initial.loop_count) {
      mask |= 255u << 12;
      value |= uint32_t(d.loop_count) << 12;
    }
    if (initial.input_buffer_0_valid && !d.input_buffer_0_valid) mask |= 1u << 20;
    if (initial.input_buffer_1_valid && !d.input_buffer_1_valid) mask |= 1u << 21;
    ContextData::UpdateWord(at, 0, mask, value);
    if (initial.output_buffer_valid && !d.output_buffer_valid) ContextData::UpdateWord(at, 1, 1u << 31, 0);
    mask = value = 0;
    if (d.input_buffer_read_offset != initial.input_buffer_read_offset) {
      mask |= 0x03FFFFFFu;
      value |= uint32_t(d.input_buffer_read_offset);
    }
    if (d.error_status != initial.error_status) {
      mask |= 31u << 26;
      value |= uint32_t(d.error_status) << 26;
    }
    ContextData::UpdateWord(at, 2, mask, value);
    if (d.current_buffer != initial.current_buffer) ContextData::UpdateWord(at, 4, 1u << 31, uint32_t(d.current_buffer) << 31);
  }

  void Consume(Ring* ring, const ContextData* d) {
    if (!remaining_subframes) return;
    const uint8_t total = uint8_t((kBytesPerFrameChannel / kOutputBytesPerBlock) << d->is_stereo);
    if (loop_output_limit > 0 && uint8_t(total - remaining_subframes) >= loop_output_limit) {
      remaining_output_blocks -= d->output_buffer_padding;
      remaining_subframes = 0;
      loop_output_limit = 0;
      return;
    }
    const uint8_t sdc = uint8_t(std::max<uint32_t>(1, d->subframe_decode_count));
    int32_t write = std::min<int32_t>(remaining_subframes, sdc);
    if (loop_output_limit > 0) write = std::min<int32_t>(write, loop_output_limit - (total - remaining_subframes));
    const int32_t read_at = total - remaining_subframes;
    ring->Write(raw.data() + kOutputBytesPerBlock * read_at, uint32_t(write) * kOutputBytesPerBlock);
    const int32_t headroom = remaining_subframes - write == 0 ? d->output_buffer_padding : 0;
    remaining_output_blocks -= write + headroom;
    remaining_subframes = uint8_t(remaining_subframes - write);
  }

  // 1 when the codec was (re)opened for a new format, 0 when kept, -1 on failure.
  int PrepareCodec(uint32_t rate_id, bool stereo) {
    const int rate = kSampleRates[std::min<uint32_t>(rate_id, 3)];
    const int channels = stereo ? 2 : 1;
    if (codec_context && codec_context->sample_rate == rate && codec_context->channels == channels) return 0;
    if (codec_context) avcodec_free_context(&codec_context);
    codec_context = avcodec_alloc_context3(codec);
    if (!codec_context) return -1;
    codec_context->sample_rate = rate;
    codec_context->channels = channels;
    codec_context->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
    return avcodec_open2(codec_context, codec, nullptr) < 0 ? -1 : 1;
  }

  bool DecodeFrame(uint32_t frame_size, uint32_t padding) {
    packet->data = xma_frame.data();
    packet->size = int(1 + (padding + frame_size) / 8 + ((padding + frame_size) % 8 ? 1 : 0));
    const uint32_t padding_end = uint32_t(packet->size) * 8 - (8 + padding + frame_size);
    xma_frame[0] = uint8_t(((padding & 7) << 5) | ((padding_end & 7) << 2));
    if (avcodec_send_packet(codec_context, packet) < 0) return false;
    return avcodec_receive_frame(codec_context, frame) >= 0;
  }

  // Planar floats to big-endian 16-bit samples, interleaved, saturated.
  static void Convert(const uint8_t* const* planes, bool stereo, uint8_t* out) {
    auto* samples = reinterpret_cast<uint16_t*>(out);
    uint32_t o = 0;
    for (uint32_t i = 0; i < kSamplesPerFrame; ++i) {
      for (uint32_t c = 0; c <= uint32_t(stereo); ++c) {
        const float v = std::clamp(reinterpret_cast<const float*>(planes[c])[i], -1.0f, 1.0f) * 32767.0f;
        samples[o++] = std::byteswap(uint16_t(int16_t(v)));
      }
    }
  }

  void Decode(ContextData* d) {
    if (!d->AnyInputValid() || remaining_subframes > 0) return;
    if (!d->CurrentInputValid()) {
      SwapInput(d);
      if (!d->CurrentInputValid()) return;
    }
    const uint8_t* input_buffer = Physical(d->InputAddress(d->current_buffer));
    input.fill(0);
    bool loop_end_frame = false;
    if (d->loop_count > 0) loop_end_frame = d->input_buffer_read_offset == std::max(kBitsPerPacketHeader, uint32_t(d->loop_end));
    if (!d->output_buffer_block_count) return;
    if (d->input_buffer_read_offset < kBitsPerPacketHeader) d->input_buffer_read_offset = kBitsPerPacketHeader;
    const uint32_t input_size = d->InputPackets(d->current_buffer) * kBytesPerPacket;
    const uint32_t packet_count = input_size / kBytesPerPacket;
    if (d->input_buffer_read_offset >= input_size * 8) return;
    const uint32_t packet_index = (d->input_buffer_read_offset >> 3) / kBytesPerPacket;
    const uint8_t* packet_data = input_buffer + packet_index * kBytesPerPacket;
    const uint32_t first_frame = PacketFrameOffset(packet_data);
    uint32_t relative = d->input_buffer_read_offset % kBitsPerPacket;
    if (relative < first_frame) {
      d->input_buffer_read_offset = packet_index * kBitsPerPacket + first_frame;
      relative = first_frame;
    }
    const uint8_t skip = PacketSkipCount(packet_data);
    if (skip == 0xFF) {  // no frame starts in this packet
      const uint32_t next = NextPacketReadOffset(input_buffer, packet_index + 1, packet_count);
      if (next == kBitsPerPacketHeader) SwapInput(d);
      d->input_buffer_read_offset = next;
      return;
    }
    PacketInfo info = Info(packet_data, relative);
    const uint32_t next_packet_index = packet_index + skip + 1;
    if (info.current_frame_size == 0) {  // the frame header spans into the next packet
      const uint8_t* next = NextPacket(d, next_packet_index, packet_count);
      if (!next) {
        SwapInput(d);
        return;
      }
      std::memcpy(input.data(), packet_data + kBytesPerPacketHeader, kBytesPerPacketData);
      std::memcpy(input.data() + kBytesPerPacketData, next + kBytesPerPacketHeader, kBytesPerPacketData);
      BitStream combined(input.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
      combined.SetOffset(relative - kBitsPerPacketHeader);
      const uint64_t size = combined.Peek(kBitsPerFrameHeader);
      if (size == kMaxFrameLength) {
        d->error_status = 4;
        return;
      }
      info.current_frame_size = uint32_t(size);
    }
    BitStream stream(input_buffer, (packet_index + 1) * kBitsPerPacket);
    stream.SetOffset(d->input_buffer_read_offset);
    const uint64_t bits_to_copy = std::min<uint64_t>(stream.Remaining(), info.current_frame_size);
    if (bits_to_copy == 0) {
      SwapInput(d);
      return;
    }
    if (info.LastFrameInPacket() && stream.Remaining() < info.current_frame_size) {
      const uint8_t* next = NextPacket(d, next_packet_index, packet_count);
      if (!next) {
        d->error_status = 4;
        return;
      }
      std::memcpy(input.data() + kBytesPerPacketData, next + kBytesPerPacketHeader, kBytesPerPacketData);
    }
    std::memcpy(input.data(), packet_data + kBytesPerPacketHeader, kBytesPerPacketData);
    BitStream frame_stream(input.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
    frame_stream.SetOffset(relative - kBitsPerPacketHeader);
    xma_frame.fill(0);
    const uint32_t padding = uint32_t(frame_stream.Copy(xma_frame.data() + 1, info.current_frame_size));
    decoded.fill(0);
    const int prepared = PrepareCodec(d->sample_rate, d->is_stereo);
    if (prepared == 1) carry_valid = false;  // the carried tail belongs to the previous stream
    if (prepared >= 0 && DecodeFrame(info.current_frame_size, padding)) {
      Convert(frame->data, d->is_stereo, decoded.data());
      // Realign onto the bitstream's numbering: the frame's samples start kDecoderStartPadding into this block and
      // end in the head of the next one.
      const size_t pad_bytes = size_t(kDecoderStartPadding * kBytesPerSample) << d->is_stereo;
      const size_t carry_bytes = (size_t(kBytesPerFrameChannel) << d->is_stereo) - pad_bytes;
      const uint8_t output_limit = loop_end_frame ? uint8_t((d->loop_subframe_end + 1) << d->is_stereo) : 0;
      const uint8_t start_skip = loop_start_skip_pending ? uint8_t(d->loop_subframe_skip << d->is_stereo) : 0;
      loop_start_skip_pending = false;
      if (carry_valid) {
        std::memcpy(raw.data(), carry.data(), carry_bytes);
        std::memcpy(raw.data() + carry_bytes, decoded.data(), pad_bytes);
        remaining_subframes = uint8_t(4 << d->is_stereo);
        loop_output_limit = pending_output_limit;
        remaining_subframes = uint8_t(remaining_subframes - std::min(pending_start_skip, remaining_subframes));
      }
      std::memcpy(carry.data(), decoded.data() + pad_bytes, carry_bytes);
      carry_valid = true;
      pending_output_limit = output_limit;
      pending_start_skip = start_skip;
    } else {
      carry_valid = false;  // a dropped frame breaks the carry's adjacency
    }
    if (loop_end_frame) {
      UpdateLoop(d);
      return;
    }
    if (!info.LastFrameInPacket()) {
      const uint32_t next_frame = uint32_t((d->input_buffer_read_offset + bits_to_copy) % kBitsPerPacket);
      d->input_buffer_read_offset = packet_index * kBitsPerPacket + next_frame;
      return;
    }
    uint32_t next = NextPacketReadOffset(input_buffer, next_packet_index, packet_count);
    if (next == kBitsPerPacketHeader) {
      SwapInput(d);
      if (d->AnyInputValid()) {
        next = PacketFrameOffset(Physical(d->InputAddress(d->current_buffer)));
        if (next > kMaxFrameSizeInBits) {
          SwapInput(d);
          return;
        }
      }
    }
    d->input_buffer_read_offset = next;
  }

  // One kick's worth of decoding; false when the context had nothing to do.
  bool Work() {
    if (!enabled.load(std::memory_order_acquire)) return false;
    std::lock_guard guard(lock);
    if (!enabled.exchange(false, std::memory_order_acq_rel)) return false;
    uint8_t* at = Guest32(address);
    ContextData data(at);
    const ContextData initial = data;
    if (!data.output_buffer_valid) return true;
    Ring ring = OutputRing(&data);
    if (!ring.capacity) return true;
    if (data.ConsumeOnly()) {
      if (remaining_subframes == 0) return true;
      Consume(&ring, &data);
      data.output_buffer_write_offset = ring.write / kOutputBytesPerBlock;
      if (ring.empty()) data.output_buffer_valid = 0;
      StoreMerged(data, initial, at);
      return true;
    }
    const int32_t minimum = int32_t(std::max<uint32_t>(1, data.subframe_decode_count)) + data.output_buffer_padding;
    if (minimum > remaining_output_blocks) {
      StoreMerged(data, initial, at);
      return true;
    }
    while (remaining_output_blocks >= minimum) {
      const uint32_t before = data.input_buffer_read_offset;
      const uint8_t pending = remaining_subframes;
      Decode(&data);
      Consume(&ring, &data);
      // Keep draining a frame the input ran out under: nothing else would deliver it.
      if ((!data.AnyInputValid() || data.error_status == 4) && remaining_subframes == 0) break;
      if (pending == 0 && data.input_buffer_read_offset == before && remaining_subframes == 0) break;  // no progress
    }
    if (initial.AnyInputValid()) {
      data.output_buffer_write_offset = ring.write / kOutputBytesPerBlock;
    } else if (data.output_buffer_write_offset != data.output_buffer_read_offset) {
      // Starved: titles take write == read as their stall sign.
      data.output_buffer_write_offset = data.output_buffer_read_offset;
      data.output_buffer_valid = 0;
    }
    if (remaining_output_blocks == 0 && ring.empty()) data.output_buffer_valid = 0;  // full, not merely empty
    StoreMerged(data, initial, at);
    return true;
  }
};

XmaDecoder::XmaDecoder() = default;

bool XmaDecoder::Available() { return avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES) != nullptr; }

XmaDecoder::~XmaDecoder() { Stop(); }

void XmaDecoder::Start(Guest g) {
  Stop();
  guest_ = g;
  running_ = true;
  worker_ = std::thread([this] { Run(); });
}

void XmaDecoder::Stop() {
  {
    std::lock_guard guard(wake_mutex_);
    running_ = false;
    woken_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

XmaDecoder::Context* XmaDecoder::Find(uint32_t context) {
  std::lock_guard guard(contexts_mutex_);
  const auto it = contexts_.find(context);
  return it == contexts_.end() ? nullptr : it->second.get();
}

void XmaDecoder::ContextAllocated(Guest g, uint32_t context) {
  std::lock_guard guard(contexts_mutex_);
  if (!guest_.base) guest_ = g;
  auto& slot = contexts_[context];
  if (!slot) slot = std::make_unique<Context>(g, context);
  slot->enabled = false;
  slot->ResetDecoder();
}

void XmaDecoder::ContextReleased(Guest, uint32_t context) {
  if (Context* c = Find(context)) {
    std::lock_guard guard(c->lock);
    c->enabled = false;
    c->ResetDecoder();
  }
}

void XmaDecoder::ContextEnabled(Guest, uint32_t context) {
  // As the reference: a kick decodes what it can at once; the title kicks again as it reads the output.
  if (Context* c = Find(context)) {
    c->enabled = true;
    c->Work();
  }
  {
    std::lock_guard guard(wake_mutex_);
    woken_ = true;
  }
  wake_.notify_one();
}

void XmaDecoder::ContextDisabled(Guest, uint32_t context) {
  if (Context* c = Find(context)) {
    c->enabled = false;
    std::lock_guard guard(c->lock);  // a decode in progress ends first
  }
}

void XmaDecoder::ContextInitialized(Guest, uint32_t context) {
  if (Context* c = Find(context)) c->Clear();
}

bool XmaDecoder::WorkOnce() {
  std::vector<Context*> all;
  {
    std::lock_guard guard(contexts_mutex_);
    for (auto& [address, c] : contexts_) all.push_back(c.get());
  }
  bool worked = false;
  for (Context* c : all) worked = c->Work() || worked;
  return worked;
}

void XmaDecoder::Run() {
  while (true) {
    {
      std::unique_lock guard(wake_mutex_);
      if (!running_) return;
    }
    if (WorkOnce()) continue;
    std::unique_lock guard(wake_mutex_);
    wake_.wait_for(guard, std::chrono::milliseconds(2), [this] { return woken_ || !running_; });
    woken_ = false;
  }
}

}  // namespace any360::sys
