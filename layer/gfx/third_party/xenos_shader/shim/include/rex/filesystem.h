// any360 shim for <rex/filesystem.h>: only OpenFile, used by Shader::DumpUcode / Translation::Dump.
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace rex::filesystem {

inline FILE* OpenFile(const std::filesystem::path& path, const std::string_view mode) {
#if defined(_WIN32)
  std::wstring wmode(mode.begin(), mode.end());
  return _wfopen(path.c_str(), wmode.c_str());
#else
  return std::fopen(path.c_str(), std::string(mode).c_str());
#endif
}

}  // namespace rex::filesystem
