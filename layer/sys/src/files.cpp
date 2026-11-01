#include "any360/sys/files.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <system_error>

#include "any360/sys/objects.h"

namespace any360::sys {

namespace {

constexpr uint32_t kStatusSuccess = 0,
                   kStatusAccessDenied = 0xC0000022;
constexpr uint32_t kGenericRead = 0x80000000, kGenericWrite = 0x40000000, kGenericAll = 0x10000000, kFileWriteData = 0x2,
                   kFileAppendData = 0x4, kDelete = 0x10000;
constexpr uint32_t kRootDosDevices = 0xFFFFFFFD;
constexpr uint64_t kCluster = 0x4000;

std::string Lower(std::string s) {
  for (char& c : s) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
  return s;
}

std::vector<std::string> Split(const std::string& path) {
  std::vector<std::string> parts;
  std::string part;
  for (char c : path) {
    if (c == '\\' || c == '/') {
      if (!part.empty()) parts.push_back(part);
      part.clear();
    } else {
      part += c;
    }
  }
  if (!part.empty()) parts.push_back(part);
  return parts;
}

std::string AnsiString(Guest g, uint32_t string) {
  if (!string) return {};
  const uint16_t length = g.Load16(string);
  const uint32_t pointer = g.Load32(string + 4);
  if (!pointer) return {};
  return std::string(reinterpret_cast<const char*>(g.host(pointer)), length);
}

bool Match(const std::string& pattern, const std::string& name) {
  // '*' and '?' wildcards, case-insensitive; "*.*" matches names without a dot too, as on the console.
  if (pattern == "*" || pattern == "*.*") return true;
  const std::string p = Lower(pattern), n = Lower(name);
  size_t pi = 0, ni = 0, star = std::string::npos, mark = 0;
  while (ni < n.size()) {
    if (pi < p.size() && (p[pi] == '?' || p[pi] == n[ni])) {
      ++pi;
      ++ni;
    } else if (pi < p.size() && p[pi] == '*') {
      star = pi++;
      mark = ni;
    } else if (star != std::string::npos) {
      pi = star + 1;
      ni = ++mark;
    } else {
      return false;
    }
  }
  while (pi < p.size() && p[pi] == '*') ++pi;
  return pi == p.size();
}

uint64_t FileTime(const std::filesystem::path& path) {
  std::error_code error;
  const auto written = std::filesystem::last_write_time(path, error);
  if (error) return 0;
  const auto system = std::chrono::clock_cast<std::chrono::system_clock>(written);
  const auto units = std::chrono::duration_cast<std::chrono::nanoseconds>(system.time_since_epoch()).count() / 100;
  return uint64_t(units) + 116444736000000000ull;
}

struct Stat {
  uint64_t time = 0, size = 0, allocation = 0;
  uint32_t attributes = 0;
};

Stat StatOf(const std::filesystem::path& path) {
  Stat s;
  std::error_code error;
  const bool directory = std::filesystem::is_directory(path, error);
  s.time = FileTime(path);
  s.size = directory ? 0 : uint64_t(std::filesystem::file_size(path, error));
  if (error) s.size = 0;
  s.allocation = (s.size + kCluster - 1) / kCluster * kCluster;
  s.attributes = directory ? kAttributeDirectory : kAttributeNormal;
  return s;
}

// The 56-byte FILE_NETWORK_OPEN_INFORMATION.
void StoreNetworkOpen(Guest g, uint32_t info, const Stat& s) {
  for (uint32_t i = 0; i < 4; ++i) g.Store64(info + 8 * i, s.time);
  g.Store64(info + 32, s.allocation);
  g.Store64(info + 40, s.size);
  g.Store32(info + 48, s.attributes);
  g.Store32(info + 52, 0);
}

void StoreStatus(Guest g, uint32_t io_status, uint32_t status, uint32_t information) {
  if (!io_status) return;
  g.Store32(io_status, status);
  g.Store32(io_status + 4, information);
}

}  // namespace

FileSystem& Files() {
  static FileSystem files;
  return files;
}

FileSystem::FileSystem() { Reset(); }

void FileSystem::Reset() {
  mounts_.clear();
  links_.clear();
  open_.clear();
  Objects().SetCloseHook([](uint32_t object) { Files().OnClose(object); });
}

void FileSystem::Mount(const std::string& device, const std::filesystem::path& host, bool read_only) {
  mounts_.push_back({Lower(device), host, read_only});
  // Longest device paths first, so "\device\harddisk0\partition1" wins over "\device\harddisk0".
  std::sort(mounts_.begin(), mounts_.end(), [](const Mounted& a, const Mounted& b) { return a.device.size() > b.device.size(); });
}

void FileSystem::Link(const std::string& link, const std::string& target) {
  std::string name = Lower(link);
  if (name.rfind("\\??\\", 0) == 0) name = name.substr(4);
  links_[name] = target;
}

void FileSystem::Unmount(const std::string& device) {
  const std::string lower = Lower(device);
  mounts_.erase(std::remove_if(mounts_.begin(), mounts_.end(), [&](const Mounted& m) { return m.device == lower; }), mounts_.end());
}

void FileSystem::Unlink(const std::string& link) {
  std::string name = Lower(link);
  if (name.rfind("\\??\\", 0) == 0) name = name.substr(4);
  links_.erase(name);
}

uint32_t FileSystem::CreateSymbolicLink(Guest g, uint32_t link, uint32_t target) {
  const std::string name = AnsiString(g, link), to = AnsiString(g, target);
  if (name.empty() || to.empty()) return kStatusInvalidParameter;
  Link(name, to);
  return kStatusSuccess;
}

uint32_t FileSystem::DeleteSymbolicLink(Guest g, uint32_t link) {
  std::string name = Lower(AnsiString(g, link));
  if (name.rfind("\\??\\", 0) == 0) name = name.substr(4);
  return links_.erase(name) ? kStatusSuccess : kStatusObjectNameNotFound;
}

// A guest path with links expanded and "\??\" removed: "\device\cdrom0\media\a.bin" (case kept for the components).
std::string FileSystem::Canonical(const std::string& guest_path) const {
  std::string path = guest_path;
  if (path.rfind("\\??\\", 0) == 0) path = path.substr(4);
  const size_t colon = path.find(':');
  const size_t slash = path.find_first_of("\\/");
  if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) {
    const auto it = links_.find(Lower(path.substr(0, colon + 1)));
    if (it == links_.end()) return {};
    path = it->second + "\\" + path.substr(colon + 1);
  }
  return path;
}

const FileSystem::Mounted* FileSystem::MountOf(const std::string& device_path, std::string& rest) const {
  const std::string lower = Lower(device_path);
  for (const Mounted& m : mounts_) {
    if (lower.rfind(m.device, 0) == 0 && (lower.size() == m.device.size() || lower[m.device.size()] == '\\' || lower[m.device.size()] == '/')) {
      rest = device_path.substr(m.device.size());
      return &m;
    }
  }
  return nullptr;
}

std::filesystem::path FileSystem::Resolve(const std::string& guest_path, bool* parent_exists) const {
  if (parent_exists) *parent_exists = false;
  std::string rest;
  const Mounted* m = MountOf(Canonical(guest_path), rest);
  if (!m) return {};
  std::filesystem::path host = m->host;
  const std::vector<std::string> parts = Split(rest);
  for (size_t i = 0; i < parts.size(); ++i) {
    const std::string& part = parts[i];
    if (part == ".") continue;
    if (part == "..") return {};  // never above the device's root
    const bool last = i + 1 == parts.size();
    std::error_code error;
    std::filesystem::path next = host / part;
    if (!std::filesystem::exists(next, error)) {
      // Case-insensitive match among the directory's entries.
      bool found = false;
      for (const auto& entry : std::filesystem::directory_iterator(host, error)) {
        if (Lower(entry.path().filename().string()) == Lower(part)) {
          next = entry.path();
          found = true;
          break;
        }
      }
      if (!found && !last) return {};
    }
    host = next;
  }
  if (parent_exists) *parent_exists = true;
  return host;
}

std::string FileSystem::PathOf(Guest g, uint32_t attributes) const {
  std::string path = AnsiString(g, g.Load32(attributes + 4));
  const uint32_t root = g.Load32(attributes);
  if (root && root != kRootDosDevices) {
    auto it = open_.find(Objects().Lookup(root));
    if (it == open_.end()) return {};
    path = it->second.guest + "\\" + path;
  }
  return path;
}

FileSystem::Open* FileSystem::Find(uint32_t handle) {
  auto it = open_.find(Objects().Lookup(handle));
  return it == open_.end() ? nullptr : &it->second;
}

uint32_t FileSystem::CreateFile(Guest g, uint32_t handle_out, uint32_t access, uint32_t attributes, uint32_t io_status, uint32_t,
                                uint32_t disposition, uint32_t options) {
  if (!attributes) return kStatusInvalidParameter;
  const std::string guest = PathOf(g, attributes);
  for (char c : guest) {
    if (uint8_t(c) < 32 || uint8_t(c) >= 127 || c == '"' || c == '<' || c == '>' || c == '|') return kStatusObjectNameInvalid;
  }
  auto fail = [&](uint32_t status, uint32_t action) {
    StoreStatus(g, io_status, status, action);
    if (handle_out) g.Store32(handle_out, 0xFFFFFFFF);
    return status;
  };
  std::string rest;
  const Mounted* mount = MountOf(Canonical(guest), rest);
  bool parent = false;
  const std::filesystem::path host = Resolve(guest, &parent);
  if (!mount || !parent) return fail(kStatusObjectPathNotFound, kDoesNotExist);
  if (access & kGenericRead) access |= 0x1;
  if (access & (kGenericWrite | kGenericAll)) access |= kFileWriteData;
  const bool wants_write = (access & (kFileWriteData | kFileAppendData | kDelete)) || disposition != kOpen || (options & kFileDeleteOnClose);
  std::error_code error;
  const bool exists = std::filesystem::exists(host, error);
  const bool directory = exists && std::filesystem::is_directory(host, error);
  if (exists && directory && (options & kFileNonDirectoryFile)) return fail(kStatusFileIsADirectory, kExists);
  if (exists && !directory && (options & kFileDirectoryFile)) return fail(kStatusNotADirectory, kExists);
  if (!exists && (disposition == kOpen || disposition == kOverwrite)) return fail(kStatusObjectNameNotFound, kDoesNotExist);
  if (exists && disposition == kCreate) return fail(kStatusObjectNameCollision, kExists);
  if (mount->read_only && wants_write && !(exists && disposition == kOpenIf && !(access & (kFileWriteData | kFileAppendData | kDelete)))) {
    return fail(kStatusAccessDenied, exists ? kExists : kDoesNotExist);
  }
  uint32_t action = kOpened;
  if (!exists) {
    if (options & kFileDirectoryFile) std::filesystem::create_directory(host, error);
    else std::ofstream(host, std::ios::binary).close();
    if (error) return fail(kStatusAccessDenied, kDoesNotExist);
    action = kCreated;
  } else if (!directory && (disposition == kSupersede || disposition == kOverwrite || disposition == kOverwriteIf)) {
    std::filesystem::resize_file(host, 0, error);
    action = disposition == kSupersede ? kSuperseded : kOverwritten;
  }
  const uint32_t object = Objects().AllocateObject(16);
  if (!object) return fail(0xC0000017, kDoesNotExist);
  std::memset(g.host(object), 0, 16);
  Open file;
  file.host = host;
  file.guest = guest;
  file.mount = mount;
  file.directory = std::filesystem::is_directory(host, error);
  file.delete_on_close = (options & kFileDeleteOnClose) != 0;
  open_[object] = file;
  const uint32_t handle = Objects().OpenHandle(object);
  if (handle_out) g.Store32(handle_out, handle);
  StoreStatus(g, io_status, kStatusSuccess, action);
  return kStatusSuccess;
}

uint32_t FileSystem::Transfer(Guest g, uint32_t handle, uint32_t event, uint32_t io_status, uint32_t buffer, uint32_t length,
                              uint32_t offset_ptr, bool write) {
  if (event && !Objects().Lookup(event)) return kStatusInvalidHandle;
  Open* file = Find(handle);
  if (!file) return kStatusInvalidHandle;
  if (file->directory) return kStatusInvalidParameter;
  uint64_t offset = offset_ptr ? g.Load64(offset_ptr) : ~0ull;
  if (offset >= 0xFFFFFFFFFFFFFFFEull) offset = file->position;  // FILE_USE_FILE_POINTER_POSITION
  uint32_t done = 0, status = kStatusSuccess;
  if (write) {
    if (file->mount->read_only) return kStatusAccessDenied;
    std::fstream stream(file->host, std::ios::binary | std::ios::in | std::ios::out);
    stream.seekp(std::streamoff(offset));
    stream.write(reinterpret_cast<const char*>(g.host(buffer)), length);
    done = stream ? length : 0;
    if (!stream) status = kStatusAccessDenied;
  } else {
    std::error_code error;
    const uint64_t size = std::filesystem::file_size(file->host, error);
    if (length && offset >= size) {
      status = kStatusEndOfFile;
    } else if (length) {
      std::ifstream stream(file->host, std::ios::binary);
      stream.seekg(std::streamoff(offset));
      const uint32_t n = uint32_t(std::min<uint64_t>(length, size - offset));
      stream.read(reinterpret_cast<char*>(g.host(buffer)), n);
      done = uint32_t(stream.gcount());
    }
  }
  file->position = offset + done;
  StoreStatus(g, io_status, status, done);
  if (event) Objects().NtSetEvent(g, event, 0);
  return status;
}

uint32_t FileSystem::ReadFile(Guest g, uint32_t handle, uint32_t event, uint32_t io_status, uint32_t buffer, uint32_t length,
                              uint32_t offset_ptr) {
  return Transfer(g, handle, event, io_status, buffer, length, offset_ptr, false);
}

uint32_t FileSystem::WriteFile(Guest g, uint32_t handle, uint32_t event, uint32_t io_status, uint32_t buffer, uint32_t length,
                               uint32_t offset_ptr) {
  return Transfer(g, handle, event, io_status, buffer, length, offset_ptr, true);
}

uint32_t FileSystem::QueryInformation(Guest g, uint32_t handle, uint32_t io_status, uint32_t info, uint32_t length, uint32_t info_class) {
  uint32_t minimum;
  switch (info_class) {
    case kFileBasicInformation: minimum = 40; break;
    case kFileStandardInformation: minimum = 24; break;
    case kFileInternalInformation: case kFilePositionInformation: minimum = 8; break;
    case kFileModeInformation: case kFileAlignmentInformation: minimum = 4; break;
    case kFileNetworkOpenInformation: minimum = 56; break;
    default: return kStatusInvalidInfoClass;
  }
  if (length < minimum) return kStatusInfoLengthMismatch;
  Open* file = Find(handle);
  if (!file) return kStatusInvalidHandle;
  std::memset(g.host(info), 0, length);
  const Stat s = StatOf(file->host);
  switch (info_class) {
    case kFileBasicInformation:
      for (uint32_t i = 0; i < 4; ++i) g.Store64(info + 8 * i, s.time);
      g.Store32(info + 32, s.attributes);
      break;
    case kFileStandardInformation:
      g.Store64(info, s.allocation);
      g.Store64(info + 8, s.size);
      g.Store32(info + 16, 1);
      g.Store8(info + 20, file->delete_on_close ? 1 : 0);
      g.Store8(info + 21, file->directory ? 1 : 0);
      break;
    case kFileInternalInformation:
      g.Store64(info, std::hash<std::string>{}(Lower(Canonical(file->guest))));
      break;
    case kFilePositionInformation:
      g.Store64(info, file->position);
      break;
    case kFileNetworkOpenInformation:
      StoreNetworkOpen(g, info, s);
      break;
    default:
      break;  // mode and alignment: 0
  }
  StoreStatus(g, io_status, kStatusSuccess, minimum);
  return kStatusSuccess;
}

uint32_t FileSystem::SetInformation(Guest g, uint32_t handle, uint32_t io_status, uint32_t info, uint32_t length, uint32_t info_class) {
  uint32_t minimum;
  switch (info_class) {
    case kFileBasicInformation: minimum = 40; break;
    case kFileRenameInformation: minimum = 16; break;
    case kFileDispositionInformation: minimum = 1; break;
    case kFilePositionInformation: case kFileAllocationInformation: case kFileEndOfFileInformation: minimum = 8; break;
    case kFileModeInformation: minimum = 4; break;
    default: return kStatusInvalidInfoClass;
  }
  if (length < minimum) return kStatusInfoLengthMismatch;
  Open* file = Find(handle);
  if (!file) return kStatusInvalidHandle;
  uint32_t status = kStatusSuccess;
  std::error_code error;
  switch (info_class) {
    case kFilePositionInformation:
      file->position = g.Load64(info);
      break;
    case kFileDispositionInformation:
      if (file->mount->read_only) status = kStatusAccessDenied;
      else if (file->directory && g.Load8(info) && !std::filesystem::is_empty(file->host, error)) status = kStatusDirectoryNotEmpty;
      else file->delete_on_close = g.Load8(info) != 0;
      break;
    case kFileAllocationInformation:
    case kFileEndOfFileInformation:
      if (file->mount->read_only) {
        status = kStatusAccessDenied;
      } else {
        std::filesystem::resize_file(file->host, g.Load64(info), error);
        if (error) status = kStatusAccessDenied;
      }
      break;
    case kFileRenameInformation: {
      // REPLACE_EXISTING, a root directory handle, then the new name as an ANSI_STRING (same device).
      const std::string target = AnsiString(g, info + 8);
      bool parent = false;
      const std::filesystem::path to = Resolve(target, &parent);
      std::string rest_from, rest_to;
      if (!parent || MountOf(Canonical(target), rest_to) != file->mount) {
        status = kStatusObjectPathNotFound;
      } else if (file->mount->read_only) {
        status = kStatusAccessDenied;
      } else if (std::filesystem::exists(to, error) && !g.Load32(info)) {
        status = kStatusObjectNameCollision;
      } else {
        std::filesystem::rename(file->host, to, error);
        if (error) status = kStatusAccessDenied;
        else {
          file->host = to;
          file->guest = target;
        }
      }
      break;
    }
    default:
      break;  // basic information and mode: accepted
  }
  StoreStatus(g, io_status, status, status ? 0 : minimum);
  return status;
}

uint32_t FileSystem::QueryFullAttributes(Guest g, uint32_t attributes, uint32_t info) {
  bool parent = false;
  const std::filesystem::path host = Resolve(PathOf(g, attributes), &parent);
  std::error_code error;
  if (!parent) return kStatusObjectPathNotFound;
  if (!std::filesystem::exists(host, error)) return kStatusObjectNameNotFound;
  StoreNetworkOpen(g, info, StatOf(host));
  return kStatusSuccess;
}

uint32_t FileSystem::QueryDirectory(Guest g, uint32_t handle, uint32_t io_status, uint32_t info, uint32_t length, uint32_t pattern,
                                    uint32_t restart) {
  if (length < 72) return kStatusInfoLengthMismatch;
  Open* file = Find(handle);
  if (!file) return kStatusInvalidHandle;
  if (!file->directory) return kStatusInvalidParameter;
  const std::string rule = AnsiString(g, pattern);
  // A new pattern restarts the search; the listing is taken then, sorted, so the order is the same on every host.
  if (!rule.empty() || restart || (file->listing.empty() && file->next == 0)) {
    file->pattern = rule.empty() ? (file->pattern.empty() ? "*" : file->pattern) : rule;
    file->listing.clear();
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(file->host, error)) {
      const std::string name = entry.path().filename().string();
      if (Match(file->pattern, name)) file->listing.push_back(name);
    }
    std::sort(file->listing.begin(), file->listing.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    file->next = 0;
    if (file->listing.empty()) {
      StoreStatus(g, io_status, rule.empty() ? kStatusNoMoreFiles : kStatusNoSuchFile, 0);
      return rule.empty() ? kStatusNoMoreFiles : kStatusNoSuchFile;
    }
  }
  if (file->next >= file->listing.size()) {
    StoreStatus(g, io_status, kStatusNoMoreFiles, 0);
    return kStatusNoMoreFiles;
  }
  const std::string& name = file->listing[file->next];
  if (0x40 + name.size() > length) {
    StoreStatus(g, io_status, kStatusInfoLengthMismatch, 0);
    return kStatusInfoLengthMismatch;
  }
  ++file->next;
  const Stat s = StatOf(file->host / name);
  std::memset(g.host(info), 0, 0x40);
  g.Store32(info + 0, 0);
  g.Store32(info + 4, uint32_t(file->next));
  for (uint32_t i = 0; i < 4; ++i) g.Store64(info + 8 + 8 * i, s.time);
  g.Store64(info + 0x28, s.size);
  g.Store64(info + 0x30, s.allocation);
  g.Store32(info + 0x38, s.attributes);
  g.Store32(info + 0x3C, uint32_t(name.size()));
  std::memcpy(g.host(info + 0x40), name.data(), name.size());
  StoreStatus(g, io_status, kStatusSuccess, length);
  return kStatusSuccess;
}

uint32_t FileSystem::QueryVolume(Guest g, uint32_t handle, uint32_t io_status, uint32_t info, uint32_t length, uint32_t info_class) {
  uint32_t minimum;
  switch (info_class) {
    case kFsVolumeInformation: minimum = 24; break;
    case kFsSizeInformation: minimum = 24; break;
    case kFsDeviceInformation: minimum = 8; break;
    case kFsAttributeInformation: minimum = 16; break;
    default: return kStatusInvalidInfoClass;
  }
  if (length < minimum) return kStatusInfoLengthMismatch;
  Open* file = Find(handle);
  if (!file) return kStatusInvalidHandle;
  std::memset(g.host(info), 0, length);
  uint32_t written = minimum;
  switch (info_class) {
    case kFsVolumeInformation:
      written = 17;  // up to the label, which is empty
      break;
    case kFsSizeInformation: {
      std::error_code error;
      const auto space = std::filesystem::space(file->mount->host, error);
      const uint64_t unit = 0x200 * 32;  // 32 sectors of 512 bytes
      g.Store64(info, error ? 0 : space.capacity / unit);
      g.Store64(info + 8, error ? 0 : (file->mount->read_only ? 0 : space.available / unit));
      g.Store32(info + 16, 32);
      g.Store32(info + 20, 0x200);
      break;
    }
    case kFsDeviceInformation:
      g.Store32(info, 0x7);  // FILE_DEVICE_DISK
      break;
    case kFsAttributeInformation: {
      static const char kName[] = "FATX";
      g.Store32(info, 0);
      g.Store32(info + 4, 255);
      g.Store32(info + 8, 4);
      if (length >= 12 + 4) {
        std::memcpy(g.host(info + 12), kName, 4);
        written = 16;
      }
      break;
    }
    default:
      break;
  }
  StoreStatus(g, io_status, kStatusSuccess, written);
  return kStatusSuccess;
}

uint32_t FileSystem::Flush(Guest g, uint32_t handle, uint32_t io_status) {
  if (!Find(handle)) return kStatusInvalidHandle;
  StoreStatus(g, io_status, kStatusSuccess, 0);
  return kStatusSuccess;
}

void FileSystem::OnClose(uint32_t object) {
  auto it = open_.find(object);
  if (it == open_.end()) return;
  if (it->second.delete_on_close) {
    std::error_code error;
    std::filesystem::remove(it->second.host, error);
  }
  open_.erase(it);
}

}  // namespace any360::sys
