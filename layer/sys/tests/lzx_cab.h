// LZX data from the host, for the LDI tests and benches (Windows: cabinet.dll). FCI, the host's CAB encoder,
// compresses a buffer with an LZX window of 2^15 to 2^21 bytes; the CAB's CFDATA blocks are the chunks LDIDecompress
// takes (each one 32 KiB frame, the last shorter). FDI, the host's CAB decoder, decodes the same CAB from memory (the
// host column of the benches): its allocations go through Fdi::Alloc and are counted there.
#pragma once

#if defined(_WIN32)

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fci.h>
#include <fdi.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace any360::test::cab {

using Bytes = std::vector<uint8_t>;

struct Chunk {
  Bytes data;
  uint32_t frame;
};

namespace detail {

inline void* Alloc(ULONG size) { return std::malloc(size); }
inline void Free(void* memory) { std::free(memory); }
inline INT_PTR Open(LPSTR file, int flags, int mode, int* error, void*) {
  const int handle = _open(file, flags | _O_BINARY, mode);
  if (handle == -1) *error = errno;
  return handle;
}
inline UINT Read(INT_PTR handle, void* memory, UINT size, int* error, void*) {
  const int n = _read(int(handle), memory, size);
  if (n < 0) *error = errno;
  return UINT(n);
}
inline UINT Write(INT_PTR handle, void* memory, UINT size, int* error, void*) {
  const int n = _write(int(handle), memory, size);
  if (n < 0) *error = errno;
  return UINT(n);
}
inline int Close(INT_PTR handle, int*, void*) { return _close(int(handle)); }
inline long Seek(INT_PTR handle, long distance, int origin, int*, void*) { return _lseek(int(handle), distance, origin); }
inline int Delete(LPSTR file, int*, void*) { return std::remove(file); }
inline BOOL TempFile(char* name, int size, void*) {
  char dir[MAX_PATH], path[MAX_PATH];
  GetTempPathA(MAX_PATH, dir);
  if (!GetTempFileNameA(dir, "lzx", 0, path)) return FALSE;
  DeleteFileA(path);
  if (int(std::strlen(path)) >= size) return FALSE;
  std::strcpy(name, path);
  return TRUE;
}
inline int Placed(PCCAB, LPSTR, long, BOOL, void*) { return 0; }
inline BOOL NextCabinet(PCCAB, ULONG, void*) { return FALSE; }
inline long Status(UINT, ULONG, ULONG, void*) { return 0; }
inline INT_PTR OpenInfo(LPSTR name, USHORT* date, USHORT* time, USHORT* attributes, int* error, void*) {
  *date = 0x5021;
  *time = 0;
  *attributes = 0;
  return Open(name, _O_RDONLY, 0, error, nullptr);
}

}  // namespace detail

// `data` as a CAB file of one folder compressed by FCI with an LZX window of 2^bits; empty on failure.
inline Bytes MakeCab(const Bytes& data, uint32_t bits) {
  char dir[MAX_PATH], source[MAX_PATH];
  GetTempPathA(MAX_PATH, dir);
  GetTempFileNameA(dir, "lzs", 0, source);
  if (FILE* f = std::fopen(source, "wb")) {
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
  }
  CCAB cab = {};
  cab.cb = 0x7FFFFFFF;
  cab.cbFolderThresh = 0x7FFFFFFF;
  std::snprintf(cab.szCabPath, sizeof cab.szCabPath, "%s", dir);
  std::snprintf(cab.szCab, sizeof cab.szCab, "any360_lzx_%lu_%u.cab", GetCurrentProcessId(), bits);
  ERF erf = {};
  Bytes file;
  HFCI fci = FCICreate(&erf, detail::Placed, detail::Alloc, detail::Free, detail::Open, detail::Read, detail::Write,
                       detail::Close, detail::Seek, detail::Delete, detail::TempFile, &cab, nullptr);
  if (!fci) return file;
  char name[] = "data.bin";
  const BOOL added = FCIAddFile(fci, source, name, FALSE, detail::NextCabinet, detail::Status, detail::OpenInfo,
                                TCOMPfromLZXWindow(bits));
  const BOOL flushed = added && FCIFlushCabinet(fci, FALSE, detail::NextCabinet, detail::Status);
  FCIDestroy(fci);
  DeleteFileA(source);
  const std::string path = std::string(dir) + cab.szCab;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    file.resize(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(file.data(), 1, file.size(), f) != file.size()) file.clear();
    std::fclose(f);
  }
  DeleteFileA(path.c_str());
  if (!flushed) file.clear();
  return file;
}

// The LZX chunks of a CAB's one folder (CFHEADER without a reserve, one CFFOLDER, its CFDATA blocks); empty when the
// CAB is not that.
inline std::vector<Chunk> Chunks(const Bytes& file) {
  std::vector<Chunk> chunks;
  if (file.size() < 44) return chunks;
  auto u16 = [&](size_t at) { return uint32_t(file[at]) | uint32_t(file[at + 1]) << 8; };
  auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
  if (u32(0) != 0x4643534D || u16(26) != 1 || (u16(30) & 4)) return chunks;
  const size_t folder = 36;
  if ((u16(folder + 6) & 0xF) != 3) return chunks;  // LZX
  size_t at = u32(folder);
  const uint32_t blocks = u16(folder + 4);
  for (uint32_t i = 0; i < blocks && at + 8 <= file.size(); ++i) {
    const uint32_t compressed = u16(at + 4), uncompressed = u16(at + 6);
    if (at + 8 + compressed > file.size()) return {};
    chunks.push_back({Bytes(file.begin() + long(at + 8), file.begin() + long(at + 8 + compressed)), uncompressed});
    at += 8 + compressed;
  }
  return chunks;
}

inline std::vector<Chunk> CabChunks(const Bytes& data, uint32_t bits) { return Chunks(MakeCab(data, bits)); }

// FDI decoding a CAB held in memory into a buffer, its allocations counted.
class Fdi {
 public:
  Fdi() {
    instance() = this;
    fdi_ = FDICreate(Alloc, Free, Open, Read, Write, Close, Seek, cpuUNKNOWN, &erf_);
  }
  ~Fdi() {
    if (fdi_) FDIDestroy(fdi_);
    instance() = nullptr;
  }
  // The CAB's one file into `out` (sized to hold it); false on failure.
  bool Decode(const Bytes& cab, Bytes& out) {
    cab_ = &cab;
    out_ = &out;
    written_ = 0;
    char name[] = "mem.cab", path[] = "";
    const BOOL ok = FDICopy(fdi_, name, path, 0, Notify, nullptr, this);
    return ok && written_ == out.size();
  }
  uint64_t allocations = 0, bytes = 0;

 private:
  static Fdi*& instance() {
    static Fdi* fdi = nullptr;
    return fdi;
  }
  // Handles: 1 the CAB (read), 2 the output (write).
  static void* DIAMONDAPI Alloc(ULONG size) {
    ++instance()->allocations;
    instance()->bytes += size;
    return std::malloc(size);
  }
  static void DIAMONDAPI Free(void* memory) { std::free(memory); }
  static INT_PTR DIAMONDAPI Open(char*, int, int) {
    instance()->position_ = 0;
    return 1;
  }
  static UINT DIAMONDAPI Read(INT_PTR handle, void* memory, UINT size) {
    Fdi& f = *instance();
    if (handle != 1) return UINT(-1);
    const size_t n = std::min<size_t>(size, f.cab_->size() - std::min(f.position_, f.cab_->size()));
    std::memcpy(memory, f.cab_->data() + f.position_, n);
    f.position_ += n;
    return UINT(n);
  }
  static UINT DIAMONDAPI Write(INT_PTR handle, void* memory, UINT size) {
    Fdi& f = *instance();
    if (handle != 2 || f.written_ + size > f.out_->size()) return UINT(-1);
    std::memcpy(f.out_->data() + f.written_, memory, size);
    f.written_ += size;
    return size;
  }
  static int DIAMONDAPI Close(INT_PTR) { return 0; }
  static long DIAMONDAPI Seek(INT_PTR handle, long distance, int origin) {
    Fdi& f = *instance();
    if (handle != 1) return -1;
    f.position_ = origin == SEEK_SET ? size_t(distance) : origin == SEEK_CUR ? f.position_ + distance : f.cab_->size() + distance;
    return long(f.position_);
  }
  static INT_PTR DIAMONDAPI Notify(FDINOTIFICATIONTYPE type, PFDINOTIFICATION) {
    if (type == fdintCOPY_FILE) return 2;
    if (type == fdintCLOSE_FILE_INFO) return TRUE;
    return 0;
  }

  HFDI fdi_ = nullptr;
  ERF erf_ = {};
  const Bytes* cab_ = nullptr;
  Bytes* out_ = nullptr;
  size_t position_ = 0, written_ = 0;
};

}  // namespace any360::test::cab

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif
