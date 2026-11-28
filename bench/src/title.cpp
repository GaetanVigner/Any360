#include "title.h"

#include <rex/ppc.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <vector>

#include "any360/sys/audio.h"
#include "any360/sys/exports.h"
#include "any360/sys/files.h"
#include "any360/sys/graphics.h"
#include "any360/sys/guest.h"
#include "any360/sys/memory.h"
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"
#include "gpu.h"
#include "guest_space.h"

namespace any360::bench {

namespace {

using Fn = void (*)(PPCContext& ctx, uint8_t* base);
using Clock = std::chrono::steady_clock;

// Guest addresses of the title's code (the dispatcher's table) and data (the image's data section).
constexpr uint32_t kMainEntry = 0x82000000, kRenderEntry = 0x82000010, kWorkerEntry = 0x82000020,
                   kAudioEntry = 0x82000030, kInterruptEntry = 0x82000040;
constexpr uint32_t kData = 0x83000000, kKernelHeap = 0x88000000, kKernelHeapSize = 16 << 20;
constexpr uint32_t kMaxWorkers = 4;
constexpr uint32_t kGpuRegisterRbWptr = 0x01C5;

// The data section: kernel objects and the title's words, at fixed offsets from kData.
enum : uint32_t {
  kEvJobStart = kData + 0x000,   // kMaxWorkers KEVENTs (16 bytes)
  kEvJobDone = kData + 0x100,    // kMaxWorkers KEVENTs
  kSemQueued = kData + 0x200,    // KSEMAPHORE (20 bytes): frames ready for the render thread
  kSemSlots = kData + 0x220,     // KSEMAPHORE: object slot buffers free for the main thread
  kCritical = kData + 0x240,     // RTL_CRITICAL_SECTION (28 bytes)
  kDoneArray = kData + 0x280,    // kMaxWorkers pointers to the job done events
  kHandles = kData + 0x2C0,      // thread handles: render, workers
  kAudioClient = kData + 0x300,  // {callback, argument}
  kAudioDriver = kData + 0x308,
  kZeroInterval = kData + 0x310,  // 64-bit 0: a delay that yields
  kSwapArgs = kData + 0x320,      // VdSwap's pointers' targets: front buffer, format, colour space, width, height
  kFetch = kData + 0x340,         // the front buffer's fetch constant (6 dwords)
  kAllocBase = kData + 0x360,     // NtAllocateVirtualMemory's in/out words
  kAllocSize = kData + 0x364,
  kAudioBuffer = kData + 0x1000,  // 6 x 256 floats
  kSlotsPointer = kData + 0x3000,
};

struct Title {
  TitleOptions o;
  City city{CityOptions{}};
  CityMemory mem;
  uint8_t* base = nullptr;
  Fn imports[64] = {};
  uint32_t ring = 0, ring_physical = 0, ring_dwords = 0, write_back = 0, fence = 0, fence_physical = 0;
  uint32_t command_buffers[2] = {}, command_physical[2] = {}, front = 0;
  uint32_t slots[2] = {};  // the object slot buffers (virtual)
  std::atomic<uint32_t> job_frame{0}, render_frame{0}, produced{0};
  std::atomic<bool> stop{false}, quit{false}, finished{false};
  uint64_t audio_sample = 0;
  std::atomic<uint64_t> audio_frames{0};  // frames the audio client submitted
  std::atomic<uint64_t> calls{0};
  uint64_t pm4_hash = 0xCBF29CE484222325ull, pm4_dwords = 0;
  double main_ns = 0, render_ns = 0;
  std::atomic<uint64_t> vblanks{0};
  std::vector<uint32_t> written;
};
Title* g_title = nullptr;
bool TraceOn() {
  char* value = nullptr;
  size_t size = 0;
  const bool on = _dupenv_s(&value, &size, "ANY360_BENCH_TRACE") == 0 && value;
  std::free(value);
  return on;
}
const bool g_trace = TraceOn();
void Trace(const char* what, uint32_t value) {
  if (g_trace) std::fprintf(stderr, "trace: %s %u\n", what, value), std::fflush(stderr);
}

enum Import : uint32_t {
  kNtAllocateVirtualMemory, kNtFreeVirtualMemory, kMmAllocatePhysicalMemoryEx, kMmGetPhysicalAddress, kExCreateThread,
  kKeInitializeEvent, kKeSetEvent, kKeWaitForSingleObject, kKeWaitForMultipleObjects, kKeInitializeSemaphore,
  kKeReleaseSemaphore, kKeDelayExecutionThread, kNtWaitForSingleObjectEx, kNtClose, kRtlInitializeCriticalSection,
  kRtlEnterCriticalSection, kRtlLeaveCriticalSection, kVdInitializeEngines, kVdInitializeRingBuffer,
  kVdEnableRingBufferRPtrWriteBack, kVdSetGraphicsInterruptCallback, kVdSwap, kXAudioRegisterRenderDriverClient,
  kXAudioUnregisterRenderDriverClient, kXAudioSubmitRenderDriverFrame, kKeQueryPerformanceFrequency, kImportCount
};
const char* const kImportNames[kImportCount] = {
    "NtAllocateVirtualMemory", "NtFreeVirtualMemory", "MmAllocatePhysicalMemoryEx", "MmGetPhysicalAddress", "ExCreateThread",
    "KeInitializeEvent", "KeSetEvent", "KeWaitForSingleObject", "KeWaitForMultipleObjects", "KeInitializeSemaphore",
    "KeReleaseSemaphore", "KeDelayExecutionThread", "NtWaitForSingleObjectEx", "NtClose", "RtlInitializeCriticalSection",
    "RtlEnterCriticalSection", "RtlLeaveCriticalSection", "VdInitializeEngines", "VdInitializeRingBuffer",
    "VdEnableRingBufferRPtrWriteBack", "VdSetGraphicsInterruptCallback", "VdSwap", "XAudioRegisterRenderDriverClient",
    "XAudioUnregisterRenderDriverClient", "XAudioSubmitRenderDriverFrame", "KeQueryPerformanceFrequency"};

sys::Guest G() { return sys::Guest{g_title->base}; }

// A system call as an import thunk makes it: the arguments in r3 onwards (the 9th and 10th in the caller's frame),
// the export run on the caller's context, r3 returned.
uint64_t Call(PPCContext& ctx, Import import, std::initializer_list<uint64_t> args, std::initializer_list<uint32_t> stack = {}) {
  PPCRegister* const r[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  uint32_t i = 0;
  for (uint64_t v : args) r[i++]->u64 = v;
  const uint32_t frame = stack.size() ? 0x80 : 0;
  ctx.r1.u32 -= frame;
  i = 0;
  for (uint32_t v : stack) G().Store32(ctx.r1.u32 + 0x54 + 8 * i++, v);
  g_title->imports[import](ctx, g_title->base);
  ctx.r1.u32 += frame;
  g_title->calls.fetch_add(1, std::memory_order_relaxed);
  return ctx.r3.u64;
}

uint32_t AllocatePhysical(PPCContext& ctx, uint32_t bytes) {
  const uint32_t size = (bytes + 0xFFFF) & ~0xFFFFu;
  return uint32_t(Call(ctx, kMmAllocatePhysicalMemoryEx, {0, size, 0x20000004u /* read-write, 64 KB pages */, 0, 0xFFFFFFFF, 0x10000}));
}
uint32_t Physical(PPCContext& ctx, uint32_t address) { return uint32_t(Call(ctx, kMmGetPhysicalAddress, {address})); }
void Yield(PPCContext& ctx) { Call(ctx, kKeDelayExecutionThread, {1, 0, kZeroInterval}); }
uint64_t Fnv(uint64_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}
double Ns(Clock::time_point a, Clock::time_point b) { return double(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()); }

void WorkerThread(PPCContext& ctx, uint8_t*) {
  Title& t = *g_title;
  const uint32_t w = ctx.r3.u32, n = t.o.city.objects;
  const uint32_t begin = n * w / t.o.workers, end = n * (w + 1) / t.o.workers;
  for (;;) {
    Call(ctx, kKeWaitForSingleObject, {kEvJobStart + 16 * w, 0, 1, 0, 0});
    if (t.stop.load(std::memory_order_acquire)) break;
    const uint32_t frame = t.job_frame.load(std::memory_order_acquire);
    t.city.Animate(frame, begin, end, reinterpret_cast<ObjectSlot*>(G().host(t.slots[frame & 1])));
    Call(ctx, kKeSetEvent, {kEvJobDone + 16 * w, 1, 0});
  }
}

void RenderThread(PPCContext& ctx, uint8_t*) {
  Title& t = *g_title;
  constexpr uint32_t kFrameRingDwords = 3 + 64 + 3;
  uint32_t write = 0;
  for (uint32_t frame = 0;; ++frame) {
    Call(ctx, kKeWaitForSingleObject, {kSemQueued, 0, 1, 0, 0});
    if (frame >= t.produced.load(std::memory_order_acquire)) break;
    Trace("render frame", frame);
    const Clock::time_point start = Clock::now();
    // The command buffer this frame reuses was last run two frames ago: wait for that frame's fence.
    while (frame >= 2 && G().Load32(t.fence) < frame - 1) Yield(ctx);
    const uint32_t cb = frame & 1;
    const uint32_t dwords = t.city.WriteFrame(frame, t.mem, reinterpret_cast<const ObjectSlot*>(G().host(t.slots[frame & 1])),
                                              G().host(t.command_buffers[cb]));
    Call(ctx, kKeReleaseSemaphore, {kSemSlots, 1, 1, 0});
    t.pm4_hash = Fnv(t.pm4_hash, G().host(t.command_buffers[cb]), 4 * dwords);
    t.pm4_dwords += dwords;
    // The ring: room for the frame's packets (wrapping with type-2 padding), the command buffer, the swap, the fence.
    auto room = [&] {
      const uint32_t read = G().Load32(t.write_back);
      return (read + t.ring_dwords - write - 1) % t.ring_dwords;
    };
    if (write + kFrameRingDwords > t.ring_dwords) {
      while (room() < t.ring_dwords - write) Yield(ctx);
      while (write < t.ring_dwords) G().Store32(t.ring + 4 * write++, 0x80000000u);
      write = 0;
      sys::Graphics().WriteRegister(kGpuRegisterRbWptr, 0);
    }
    const Clock::time_point waited = Clock::now();
    while (room() < kFrameRingDwords) Yield(ctx);
    const Clock::time_point resumed = Clock::now();
    auto put = [&](uint32_t v) { G().Store32(t.ring + 4 * write++, v); };
    put(3u << 30 | (2u - 1) << 16 | 0x3Fu << 8);  // INDIRECT_BUFFER
    put(t.command_physical[cb]);
    put(dwords);
    Call(ctx, kVdSwap, {t.ring + 4 * write, kFetch, 0, 0, 0, kSwapArgs, kSwapArgs + 4, kSwapArgs + 8},
         {kSwapArgs + 12, kSwapArgs + 16});
    write += 64;
    put(3u << 30 | (2u - 1) << 16 | 0x3Du << 8);  // MEM_WRITE
    put(t.fence_physical | 2);
    put(frame + 1);
    Call(ctx, kRtlEnterCriticalSection, {kCritical});
    t.render_frame.store(frame + 1, std::memory_order_release);
    Call(ctx, kRtlLeaveCriticalSection, {kCritical});
    sys::Graphics().WriteRegister(kGpuRegisterRbWptr, write);
    Trace("kicked", write);
    t.render_ns += Ns(start, Clock::now()) - Ns(waited, resumed);
  }
  // Every frame done on the GPU before the title ends.
  const uint32_t last = t.render_frame.load(std::memory_order_acquire);
  while (G().Load32(t.fence) < last) Yield(ctx);
}

void AudioCallback(PPCContext& ctx, uint8_t*) {
  Title& t = *g_title;
  City::Mix(t.audio_sample, G().host(kAudioBuffer));
  t.audio_sample += 256;
  Call(ctx, kXAudioSubmitRenderDriverFrame, {G().Load32(kAudioDriver), kAudioBuffer});
  t.audio_frames.fetch_add(1, std::memory_order_release);
}

void GraphicsInterrupt(PPCContext&, uint8_t*) { g_title->vblanks.fetch_add(1, std::memory_order_relaxed); }

void TitleMain(PPCContext& ctx, uint8_t*) {
  Title& t = *g_title;
  Trace("main start", 0);
  const CityOptions& c = t.o.city;
  // Memory: the GPU's buffers in physical memory, the object slots in virtual memory.
  t.ring = AllocatePhysical(ctx, 0x10000);
  t.ring_dwords = 0x10000 / 4;
  t.ring_physical = Physical(ctx, t.ring);
  const uint32_t misc = AllocatePhysical(ctx, 0x10000);
  t.write_back = misc, t.fence = misc + 0x100, t.fence_physical = Physical(ctx, t.fence);
  const uint32_t rects = misc + 0x1000;
  for (uint32_t i = 0; i < 2; ++i) {
    t.command_buffers[i] = AllocatePhysical(ctx, 4 * t.city.MaxFrameDwords());
    t.command_physical[i] = Physical(ctx, t.command_buffers[i]);
  }
  const uint32_t geometry = AllocatePhysical(ctx, GeometryBytes());
  t.mem.physical = GuestSpace::Get().base() + 0xA0000000u;  // the CPU's view: its writes go through the watch
  t.mem.geometry = Physical(ctx, geometry);
  for (uint32_t i = 0; i < kTextureCount; ++i) t.mem.textures[i] = Physical(ctx, AllocatePhysical(ctx, TextureBytes(i)));
  t.mem.minimap = Physical(ctx, AllocatePhysical(ctx, kMinimapSize * kMinimapSize * 4));
  t.front = AllocatePhysical(ctx, c.width * c.height * 4);
  t.mem.front = Physical(ctx, t.front);
  t.mem.rects = Physical(ctx, rects);
  for (uint32_t& slots : t.slots) {
    G().Store32(kAllocBase, 0);
    G().Store32(kAllocSize, c.objects * uint32_t(sizeof(ObjectSlot)));
    Call(ctx, kNtAllocateVirtualMemory, {kAllocBase, kAllocSize, 0x3000 /* reserve, commit */, 4, 0});
    slots = G().Load32(kAllocBase);
  }
  Trace("memory", t.ring);
  t.city.Build(t.mem);
  Trace("built", 0);
  // The GPU: engines, the ring and its read pointer write-back, the interrupt callback; the swap's arguments.
  Call(ctx, kVdInitializeEngines, {0, 0, 0});
  Call(ctx, kVdSetGraphicsInterruptCallback, {kInterruptEntry, 0});
  Call(ctx, kVdInitializeRingBuffer, {t.ring, 13});
  Call(ctx, kVdEnableRingBufferRPtrWriteBack, {t.write_back, 6});
  uint32_t fetch[6];
  t.city.FrontBufferFetch(t.front, fetch);
  for (uint32_t i = 0; i < 6; ++i) G().Store32(kFetch + 4 * i, fetch[i]);
  const uint32_t swap_args[5] = {t.front, 6, 0, c.width, c.height};
  for (uint32_t i = 0; i < 5; ++i) G().Store32(kSwapArgs + 4 * i, swap_args[i]);
  // Synchronization: job events, the frame queue and the slot buffers' semaphores, a critical section.
  for (uint32_t w = 0; w < t.o.workers; ++w) {
    Call(ctx, kKeInitializeEvent, {kEvJobStart + 16 * w, 1, 0});
    Call(ctx, kKeInitializeEvent, {kEvJobDone + 16 * w, 1, 0});
    G().Store32(kDoneArray + 4 * w, kEvJobDone + 16 * w);
  }
  Call(ctx, kKeInitializeSemaphore, {kSemQueued, 0, 3});  // up to 2 frames ready, and the stop release
  Call(ctx, kKeInitializeSemaphore, {kSemSlots, 2, 2});
  Call(ctx, kRtlInitializeCriticalSection, {kCritical});
  G().Store64(kZeroInterval, 0);
  // Audio: a render driver client mixing its own voices.
  G().Store32(kAudioClient, kAudioEntry);
  G().Store32(kAudioClient + 4, 0);
  Call(ctx, kXAudioRegisterRenderDriverClient, {kAudioClient, kAudioDriver});
  // Threads: the render thread and the animation workers.
  Call(ctx, kExCreateThread, {kHandles, 0x40000, 0, 0, kRenderEntry, 0, 0});
  for (uint32_t w = 0; w < t.o.workers; ++w) Call(ctx, kExCreateThread, {kHandles + 4 + 4 * w, 0x20000, 0, 0, kWorkerEntry, w, 0});

  for (uint32_t frame = 0; frame < t.o.frames && !t.quit.load(std::memory_order_relaxed); ++frame) {
    if (frame == t.o.warmup && t.o.on_measure) t.o.on_measure();
    Trace("main frame", frame);
    Call(ctx, kKeWaitForSingleObject, {kSemSlots, 0, 1, 0, 0});
    const Clock::time_point start = Clock::now();
    t.job_frame.store(frame, std::memory_order_release);
    for (uint32_t w = 0; w < t.o.workers; ++w) Call(ctx, kKeSetEvent, {kEvJobStart + 16 * w, 1, 0});
    // While the workers animate: the textures the CPU rewrites this frame, and allocation churn.
    t.written.clear();
    t.city.Stream(frame, t.mem, t.written);
    for (uint32_t k = 0; k < 3; ++k) {
      G().Store32(kAllocBase, 0);
      G().Store32(kAllocSize, (16u << 10) << (k + frame % 4));
      if (Call(ctx, kNtAllocateVirtualMemory, {kAllocBase, kAllocSize, 0x3000, 4, 0}) == 0) {
        const uint32_t block = G().Load32(kAllocBase);
        std::memset(G().host(block), int(frame), G().Load32(kAllocSize));
        G().Store32(kAllocSize, 0);
        Call(ctx, kNtFreeVirtualMemory, {kAllocBase, kAllocSize, 0x8000, 0});
      }
    }
    const Clock::time_point waiting = Clock::now();
    Call(ctx, kKeWaitForMultipleObjects, {t.o.workers, kDoneArray, 0, 0, 1, 0, 0, 0});
    const Clock::time_point resumed = Clock::now();
    t.produced.store(frame + 1, std::memory_order_release);
    Call(ctx, kKeReleaseSemaphore, {kSemQueued, 1, 1, 0});
    t.main_ns += Ns(start, waiting) + Ns(resumed, Clock::now());
  }
  t.stop.store(true, std::memory_order_release);
  for (uint32_t w = 0; w < t.o.workers; ++w) Call(ctx, kKeSetEvent, {kEvJobStart + 16 * w, 1, 0});
  Call(ctx, kKeReleaseSemaphore, {kSemQueued, 1, 1, 0});
  for (uint32_t i = 0; i <= t.o.workers; ++i) {
    const uint32_t handle = G().Load32(kHandles + 4 * i);
    Call(ctx, kNtWaitForSingleObjectEx, {handle, 1, 0, 0});
    Call(ctx, kNtClose, {handle});
  }
  // The audio the check hashes, mixed whatever the frame rate (a fast run ends before the audio clock reaches it).
  while (!t.quit.load(std::memory_order_relaxed) && t.audio_frames.load(std::memory_order_acquire) < kAudioHashFrames) Yield(ctx);
  Call(ctx, kXAudioUnregisterRenderDriverClient, {G().Load32(kAudioDriver)});
  t.finished.store(true, std::memory_order_release);
}

struct Entry {
  uint32_t address;
  Fn function;
};
const Entry kEntries[] = {{kMainEntry, &TitleMain}, {kRenderEntry, &RenderThread}, {kWorkerEntry, &WorkerThread},
                          {kAudioEntry, &AudioCallback}, {kInterruptEntry, &GraphicsInterrupt}};

uint64_t Dispatch(uint32_t address, uint32_t stack_top, uint32_t pcr, const uint64_t* args, uint32_t count, void*) {
  for (const Entry& e : kEntries) {
    if (e.address != address) continue;
    alignas(64) PPCContext ctx{};
    ctx.fpscr.InitHost();
    ctx.r1.u64 = stack_top;
    ctx.r13.u64 = pcr;
    PPCRegister* const registers[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    for (uint32_t i = 0; i < count && i < sys::kMaxDispatchArguments; ++i) registers[i]->u64 = args[i];
    e.function(ctx, g_title->base);
    return ctx.r3.u64;
  }
  std::fprintf(stderr, "any360_bench: no title function at %08X\n", address);
  std::abort();
}

class Sink : public sys::AudioSink {
 public:
  void Submit(uint32_t, const float* interleaved) override {
    const uint64_t n = frames_.fetch_add(1, std::memory_order_relaxed);
    if (n < kAudioHashFrames) {
      std::lock_guard lock(mutex_);
      hashed_[n] = Fnv(0xCBF29CE484222325ull, reinterpret_cast<const uint8_t*>(interleaved),
                       sizeof(float) * sys::kAudioChannels * sys::kAudioFrameSamples);
    }
  }
  uint64_t frames() const { return frames_.load(std::memory_order_relaxed); }
  uint64_t hash() {
    std::lock_guard lock(mutex_);
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint64_t v : hashed_) h = Fnv(h, reinterpret_cast<const uint8_t*>(&v), 8);
    return h;
  }

 private:
  std::atomic<uint64_t> frames_{0};
  std::mutex mutex_;
  uint64_t hashed_[kAudioHashFrames] = {};
};

}  // namespace

bool RunTitle(const TitleOptions& options, BenchGpu& gpu, TitleResult& result, std::string& error) {
  GuestSpace& space = GuestSpace::Get();
  Title title;
  title.o = options;
  title.o.workers = std::clamp(options.workers, 1u, kMaxWorkers);
  title.city = City(options.city);
  title.base = space.base();
  g_title = &title;
  for (uint32_t i = 0; i < kImportCount; ++i) {
    for (unsigned e = 0; e < sys::kExportCount; ++e) {
      if (!std::strcmp(sys::kExports[e].name, kImportNames[i])) title.imports[i] = sys::kExports[e].function;
    }
    if (!title.imports[i]) {
      error = std::string("any360::sys has no export ") + kImportNames[i];
      return false;
    }
  }
  const sys::Guest g{space.base()};
  sys::SetPhysicalHostOffset(GuestSpace::kPhysicalHostOffset);
  sys::Objects().SetGuest(g);
  sys::Objects().SetKernelHeap(kKernelHeap, kKernelHeapSize);
  sys::Objects().Reset();
  sys::Files().Reset();
  sys::Memory().Reset();
  sys::Threads().SetDispatcher(&Dispatch, nullptr);
  sys::Graphics().Reset();
  sys::Graphics().SetBackend(gpu.backend());
  Sink sink;
  sys::Audio().Reset();
  sys::Audio().SetSink(&sink);
  sys::Audio().Start();
  sys::Graphics().StartVblank(options.vblank_hz);

  uint32_t handle = 0;
  if (sys::Threads().CreateThreadHandle(g, &handle, 0x100000, 0, 0, kMainEntry, 0, 0) != 0) {
    error = "the main thread was not created";
    return false;
  }
  while (!title.finished.load(std::memory_order_acquire)) {
    if (!gpu.PumpWindow()) title.quit = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  sys::Graphics().StopVblank();
  sys::Audio().Stop();
  sys::Threads().Shutdown();

  result.frames = title.render_frame.load();
  result.pm4_hash = title.pm4_hash;
  result.pm4_dwords = title.pm4_dwords;
  result.audio_hash = sink.hash();
  result.audio_frames = sink.frames();
  result.vblanks = title.vblanks.load();
  result.write_faults = space.write_faults();
  const double frames = std::max(1u, result.frames);
  result.main_ms = title.main_ns / frames / 1e6;
  result.render_ms = title.render_ns / frames / 1e6;
  result.kernel_calls = double(title.calls.load()) / frames;
  g_title = nullptr;
  return true;
}

}  // namespace any360::bench
