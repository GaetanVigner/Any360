// A host file's creation, last access and last write times as FILETIMEs (100 ns since 1601), kept apart from
// files.cpp so the platform headers (windows.h and its CreateFile macros) stay out of it.
#include <cstdint>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace any360::sys {

bool HostFileTimes(const std::filesystem::path& path, uint64_t& creation, uint64_t& access, uint64_t& write) {
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
  auto join = [](const FILETIME& t) { return uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime; };
  creation = join(data.ftCreationTime);
  access = join(data.ftLastAccessTime);
  write = join(data.ftLastWriteTime);
  return true;
#else
  struct stat s;
  if (stat(path.c_str(), &s) != 0) return false;
  auto join = [](const timespec& t) { return uint64_t(t.tv_sec) * 10000000ull + uint64_t(t.tv_nsec) / 100 + 116444736000000000ull; };
#ifdef __APPLE__
  access = join(s.st_atimespec);
  write = join(s.st_mtimespec);
#else
  access = join(s.st_atim);
  write = join(s.st_mtim);
#endif
  creation = write;  // no creation time on every POSIX file system
  return true;
#endif
}

}  // namespace any360::sys
