// What the file layer asks of the host through the host's own interface: a file's creation, last access and last
// write times as FILETIMEs (100 ns since 1601), its size and whether it is a directory, in one question; and a file
// open for reads at any offset. Kept apart from files.cpp so the platform headers (windows.h and its CreateFile
// macros) stay out of it.
#include <cstdint>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace any360::sys {

bool HostFileStat(const std::filesystem::path& path, uint64_t& creation, uint64_t& access, uint64_t& write, uint64_t& size,
                  bool& directory) {
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
  auto join = [](const FILETIME& t) { return uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime; };
  creation = join(data.ftCreationTime);
  access = join(data.ftLastAccessTime);
  write = join(data.ftLastWriteTime);
  size = uint64_t(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
  directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  return true;
#else
  struct stat s;
  if (stat(path.c_str(), &s) != 0) return false;
  size = uint64_t(s.st_size);
  directory = S_ISDIR(s.st_mode);
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

// A host file open for reads: the host's own handle, not a stream of the C library, of which a process holds 512
// at most with MSVC's. -1 when it does not open. Whoever else opens, writes, renames or removes the file may.
intptr_t HostFileOpen(const std::filesystem::path& path) {
#ifdef _WIN32
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  return file == INVALID_HANDLE_VALUE ? -1 : intptr_t(file);
#else
  return open(path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
}

// Up to `length` bytes from `offset`, the file as it is on the host at this moment; fewer at its end. No position
// is kept: any thread may read at any offset.
uint32_t HostFileRead(intptr_t file, uint64_t offset, uint8_t* to, uint32_t length) {
  uint32_t done = 0;
  while (done < length) {
#ifdef _WIN32
    OVERLAPPED at{};
    at.Offset = DWORD(offset + done);
    at.OffsetHigh = DWORD((offset + done) >> 32);
    DWORD got = 0;
    if (!ReadFile(HANDLE(file), to + done, length - done, &got, &at) || !got) break;
#else
    const ssize_t got = pread(int(file), to + done, length - done, off_t(offset + done));
    if (got <= 0) break;
#endif
    done += uint32_t(got);
  }
  return done;
}

void HostFileClose(intptr_t file) {
#ifdef _WIN32
  CloseHandle(HANDLE(file));
#else
  close(int(file));
#endif
}

bool HostFileTimes(const std::filesystem::path& path, uint64_t& creation, uint64_t& access, uint64_t& write) {
  uint64_t size = 0;
  bool directory = false;
  return HostFileStat(path, creation, access, write, size, directory);
}

}  // namespace any360::sys
