// any360 shim for <rex/logging.h> (spdlog-backed category logging). The translator only reports errors through
// REXGPU_ERROR; they go to stderr, and the result is also visible as Shader::Translation::is_valid() == false.
#pragma once

#include <cstdio>
#include <string>

#include <fmt/format.h>

#define REXGPU_ERROR(...) \
  (void)std::fputs(("xenos_shader: " + ::fmt::format(__VA_ARGS__) + "\n").c_str(), stderr)
#define REXGPU_WARN(...) ((void)0)
#define REXGPU_INFO(...) ((void)0)
#define REXGPU_DEBUG(...) ((void)0)
#define REXGPU_TRACE(...) ((void)0)
