#include "any360/host/d3d_draw.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(any360_d3d_native_draws, true, "GPU",
                    "Any360: the D3D runtime's draw flushes run natively (docs/D3D.md, D2); off: the runtime's own code");
REXCVAR_DEFINE_BOOL(any360_d3d_records, true, "GPU",
                    "Any360: native draws write Any360's draw record instead of DRAW_INDX (any360/xdk/d3d_records.h)");

REXCVAR_DEFINE_BOOL(any360_direct_submit, true, "GPU",
                    "Any360: the title's kicks go to the command processor directly, not through the ring (docs/D3D.md, D6)");

REXCVAR_DEFINE_BOOL(any360_native_segments, true, "GPU",
                    "Any360: the kick's segments (their end packets, submit, chaining, the next one opened) native "
                    "(docs/D3D.md, D6)");

REXCVAR_DEFINE_BOOL(any360_native_tiling, true, "GPU",
                    "Any360: the tiled recording (its segment lists opened and closed, each tile's bin select handed to "
                    "the command processor) native (docs/D3D.md, D6)");

REXCVAR_DEFINE_BOOL(any360_d3d_native_refcounts, true, "GPU",
                    "Any360: D3D resources' reference counts (AddRef, Release) native (docs/D3D.md, D1)");

REXCVAR_DEFINE_BOOL(any360_d3d_native_draw_helpers, true, "GPU",
                    "Any360: native draws call the title's shader, state, misc and wait helpers (off: those draws are the "
                    "runtime's own code; a diagnostic)");

#include <atomic>

namespace any360::host::rexglue::d3d_draw {

bool NativeDraws() { return REXCVAR_GET(any360_d3d_native_draws); }
// Records while Any360's graphics are bound; the runtime's PM4 while the runtime's GPU runs.
bool DrawRecords() { return REXCVAR_GET(any360_d3d_records) && GraphicsBound(); }
bool DrawHelpers() { return REXCVAR_GET(any360_d3d_native_draw_helpers); }
bool DirectSubmit() { return REXCVAR_GET(any360_direct_submit); }
bool NativeSegments() { return REXCVAR_GET(any360_native_segments); }
bool NativeTiling() { return REXCVAR_GET(any360_native_tiling); }
bool NativeRefCounts() { return REXCVAR_GET(any360_d3d_native_refcounts); }

namespace {
std::atomic<uint64_t> g_brackets[2], g_tile_runs[2];
}  // namespace
void CountBracket(bool native) { g_brackets[native ? 1 : 0].fetch_add(1, std::memory_order_relaxed); }
void CountTileRun(bool native) { g_tile_runs[native ? 1 : 0].fetch_add(1, std::memory_order_relaxed); }

}  // namespace any360::host::rexglue::d3d_draw

namespace any360::host::rexglue {
uint64_t TilingBrackets(bool native) { return d3d_draw::g_brackets[native ? 1 : 0].load(std::memory_order_relaxed); }
uint64_t TileOpRuns(bool native) { return d3d_draw::g_tile_runs[native ? 1 : 0].load(std::memory_order_relaxed); }

}  // namespace any360::host::rexglue::d3d_draw
