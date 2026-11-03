// any360::host::rexglue: a title built on the ReXGlue runtime (stage 0) bound to Any360's layers, a piece at a time.
//
// First piece, the graphics: the title's GPU system calls (the Vd* exports below) go to any360::sys's GraphicsKernel
// instead of the runtime's, and its GPU MMIO (0x7FC80000-0x7FC8FFFF: the ring's write pointer above all) to the same
// kernel, whose backend is any360::gfx (CommandProcessor and VulkanRenderer), presenting into a window of its own.
// The rest of the kernel stays the runtime's for now: the title's guest threads are the runtime's, so the graphics
// interrupt callback runs on a runtime host thread (an interrupt dispatch), not on a context of the layer's. The end
// state is the whole title on Any360's layers; every piece moved here is one less the runtime provides.
//
// Use, in the title's sources:
//
//   #include "any360/host/rexglue.h"
//   ANY360_REXGLUE_GRAPHICS_EXPORTS()   // once, at namespace scope: the Vd* overrides
//   ANY360_REXGLUE_BIND_GRAPHICS(VdSwap)  // unless the title hooks VdSwap itself: its hook then calls
//                                         // any360_VdSwap when GraphicsBound(), the runtime's VdSwap otherwise
//
//   // When the runtime is set up (ReXApp::OnPostSetup), before the title's first Vd* call, when a setting asks:
//   std::string error;
//   if (!any360::host::rexglue::StartGraphics({}, error)) { /* log; the runtime's GPU stays in use */ }
//
// Until StartGraphics succeeds (and when it is never called), each override calls the runtime's own export, found in
// the runtime library by name, so one build runs either way.
#pragma once

#include <rex/ppc.h>

#include <cstdint>
#include <functional>
#include <string>

namespace any360::host::rexglue {

struct GraphicsOptions {
  std::string gpu;  // a substring of the Vulkan device name to prefer ("NVIDIA", "Radeon"); else discrete first
  uint32_t width = 1280, height = 720;  // the display mode the GPU reports, and the window's first size
  // Vertical blanks a second; 0: as the runtime's own GPU does (60 with its vsync setting on, else 1000: titles pace
  // their presents on vertical blanks, and the runtime runs them uncapped that way).
  double vblank_hz = 0.0;
  // Frames drawn inside the title's own window (the visible top-level window of the thread calling StartGraphics,
  // the runtime's UI thread), as a child covering its client area that leaves input and focus to it: the runtime's
  // input (SDL) only reads controllers while one of its windows has the focus. False: a window of its own.
  bool embed = true;
  // Fences, memory writes and interrupts of the command stream: false, as the runtime's own GPU, when the stream
  // reaches them (the CPU may see a fence before the draws before it are on screen, never waits on the GPU); true,
  // once those draws have run (what the CPU reads after them is there, at the cost of a GPU round trip each).
  bool writes_after_draws = false;
  // EVENT_WRITE_SHD fences apart: once the shaders before them are done, as on the console (the title reuses the
  // memory the GPU reads in place after them). False: when the stream reaches them, as the runtime.
  bool fences_after_draws = true;
  // The renderer on a thread of its own (gfx::AsyncRenderer): the command processor reads the stream meanwhile.
  bool async_renderer = true;
  // Threads recording draws (secondary command buffers): -1 as many as the machine leaves (AutoRecordThreads), 0 none.
  int record_threads = -1;
  // A file to write samples of the command processor's thread to (module offsets of the executable, while the
  // diagnostics are off: what it costs without them), for llvm-symbolizer. Empty: no sampling.
  std::string profile_path;
  // Where the binding reports, every `stats_seconds`: frames per second and the renderer's cost per frame, the
  // draws it could not draw by reason. None: nothing reported.
  std::function<void(const std::string&)> log;
  double stats_seconds = 5.0;
};

// Binds the graphics to Any360: makes the window, the Vulkan device and renderer, the command processor; takes over
// the GPU MMIO range; starts the command processor's thread, the interrupt thread and vertical blanks. False with
// `error` when something is missing (no Vulkan 1.3 device, no window): nothing is bound then.
bool StartGraphics(const GraphicsOptions& options, std::string& error);
// Stops the threads (call before the runtime goes away).
void StopGraphics();
// Whether the Vd* overrides go to Any360 (fixed by the first StartGraphics, before the title's first Vd* call).
bool GraphicsBound();

using GuestFunction = void (*)(PPCContext&, uint8_t*);
// The runtime's own implementation of an export (`__imp__VdSwap`), looked up in its library by name; null when absent.
GuestFunction ReferenceExport(const char* symbol);
// Runs `any360` when the graphics are bound, the runtime's `symbol` otherwise.
void CallGraphicsExport(const char* symbol, GuestFunction any360, PPCContext& ctx, uint8_t* base);

}  // namespace any360::host::rexglue

// One override: the title's generated code calls `__imp__<name>`, which a definition in the title takes from the
// runtime's import library.
#define ANY360_REXGLUE_BIND_GRAPHICS(name)                                                          \
  extern "C" void any360_##name(PPCContext& ctx, uint8_t* base);                                    \
  extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base) {                        \
    any360::host::rexglue::CallGraphicsExport("__imp__" #name, &any360_##name, ctx, base);          \
  }

// The GPU's system calls: the ring buffer and its read-pointer write-back, the interrupt callback, the system and
// scaler command buffers, the engines. VdSwap is bound on its own (ANY360_REXGLUE_BIND_GRAPHICS(VdSwap)), since titles
// often hook it already (frame pacing, frame counting). VdPersistDisplay stays the runtime's (it allocates guest memory,
// which the runtime still owns), and so do the display-mode queries (the runtime's window and the title agree on them).
#define ANY360_REXGLUE_GRAPHICS_EXPORTS()                                  \
  ANY360_REXGLUE_BIND_GRAPHICS(VdInitializeEngines)                        \
  ANY360_REXGLUE_BIND_GRAPHICS(VdShutdownEngines)                          \
  ANY360_REXGLUE_BIND_GRAPHICS(VdInitializeRingBuffer)                     \
  ANY360_REXGLUE_BIND_GRAPHICS(VdEnableRingBufferRPtrWriteBack)            \
  ANY360_REXGLUE_BIND_GRAPHICS(VdSetGraphicsInterruptCallback)             \
\
  ANY360_REXGLUE_BIND_GRAPHICS(VdGetSystemCommandBuffer)                   \
  ANY360_REXGLUE_BIND_GRAPHICS(VdSetSystemCommandBufferGpuIdentifierAddress) \
  ANY360_REXGLUE_BIND_GRAPHICS(VdInitializeScalerCommandBuffer)            \
  ANY360_REXGLUE_BIND_GRAPHICS(VdCallGraphicsNotificationRoutines)

// any360::sys's VdSwap, for a title whose own __imp__VdSwap hook forwards to it when the graphics are bound.
extern "C" void any360_VdSwap(PPCContext& ctx, uint8_t* base);
