// any360 shim for <rex/graphics/flags.h> (ReXGlue GPU cvars). The translator reads one cvar, dump_shaders
// (translator.cpp, Shader::AnalyzeUcode: dump the ucode when non-empty). It is frozen at its default, empty, so no
// files are written. The other GPU cvars of the original header belong to the caches and the command processor and
// reach the translator only through constructor arguments and modification bits.
#pragma once

#include <string>

namespace rex::graphics::any360_cvars {
inline const std::string dump_shaders;
}  // namespace rex::graphics::any360_cvars

#ifndef REXCVAR_GET
#define REXCVAR_GET(name) (::rex::graphics::any360_cvars::name)
#endif
