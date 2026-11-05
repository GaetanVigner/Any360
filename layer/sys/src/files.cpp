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
  link_objects_.clear();
  open_.clear();
  Objects().AddCloseHook([](uint32_t object) { Files().OnClose(object); });
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

bool FileSystem::Knows(const std::string& guest_path) const {
  std::string rest;
  const std::string canonical = Canonical(guest_path);
  return !canonical.empty() && MountOf(canonical, rest) != nullptr;
}

bool FileSystem::KnowsAttributes(Guest g, uint32_t attributes) const {
  if (!attributes) return false;
  const uint32_t root = g.Load32(attributes);
  if (root && root != kRootDosDevices) return open_.count(Objects().Lookup(root)) != 0;
  return Knows(AnsiString(g, g.Load32(attributes + 4)));
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
  uint32_t handle = 0xFFFFFFFF, action = kDoesNotExist;
  const uint32_t status = OpenPath(g, guest, access, disposition, options, handle, action);
  StoreStatus(g, io_status, status, action);
  if (handle_out) g.Store32(handle_out, handle);
  return status;
}

uint32_t FileSystem::OpenPath(Guest g, const std::string& guest, uint32_t access, uint32_t disposition, uint32_t options,
                              uint32_t& handle_out, uint32_t& action_out) {
  auto fail = [&](uint32_t status, uint32_t action) {
    action_out = action;
    handle_out = 0xFFFFFFFF;
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
  const uint32_t object = Objects().AllocateObject(16, true);
  if (!object) return fail(0xC0000017, kDoesNotExist);
  std::memset(g.host(object), 0, 16);
  Open file;
  file.host = host;
  file.guest = guest;
  file.mount = mount;
  file.directory = std::filesystem::is_directory(host, error);
  file.delete_on_close = (options & kFileDeleteOnClose) != 0;
  open_[object] = file;
  handle_out = Objects().OpenHandle(object);
  action_out = action;
  return kStatusSuccess;
}

uint32_t FileSystem::Transfer(Guest g, uint32_t handle, uint32_t event, uint32_t io_status, uint32_t buffer, uint32_t length,
                              uint32_t offset_ptr, bool write, const uint64_t* at) {
  if (event && !Objects().EventHandleValid(event)) return kStatusInvalidHandle;
  Open* file = Find(handle);
  if (!file) return kStatusInvalidHandle;
  if (file->directory) return kStatusInvalidParameter;
  uint64_t offset = at ? *at : offset_ptr ? g.Load64(offset_ptr) : ~0ull;
  if (!at && offset >= 0xFFFFFFFFFFFFFFFEull) offset = file->position;  // FILE_USE_FILE_POINTER_POSITION
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

uint32_t FileSystem::ReadFileScatter(Guest g, uint32_t handle, uint32_t event, uint32_t io_status, uint32_t segments, uint32_t length,
                                     uint32_t offset_ptr) {
  if (event && !Objects().EventHandleValid(event)) return kStatusInvalidHandle;
  Open* file = Find(handle);
  if (!file) return kStatusInvalidHandle;
  uint64_t offset = offset_ptr ? g.Load64(offset_ptr) : ~0ull;
  if (offset >= 0xFFFFFFFFFFFFFFFEull) offset = file->position;
  std::error_code error;
  const uint64_t size = std::filesystem::file_size(file->host, error);
  uint32_t done = 0, status = kStatusSuccess;
  if (length && offset >= size) {
    status = kStatusEndOfFile;
  } else {
    std::ifstream stream(file->host, std::ios::binary);
    stream.seekg(std::streamoff(offset));
    for (uint32_t page = 0; done < length && offset + done < size; ++page) {
      const uint32_t n = uint32_t(std::min<uint64_t>({0x1000, length - done, size - offset - done}));
      stream.read(reinterpret_cast<char*>(g.host(g.Load32(segments + 4 * page))), n);
      done += uint32_t(stream.gcount());
      if (stream.gcount() != std::streamsize(n)) break;
    }
  }
  file->position = offset + done;
  StoreStatus(g, io_status, status, done);
  if (event) Objects().NtSetEvent(g, event, 0);
  return status;
}

uint32_t FileSystem::WriteFileGather(Guest g, uint32_t handle, uint32_t event, uint32_t io_status, uint32_t segments, uint32_t length,
                                     uint32_t offset_ptr) {
  if (event && !Objects().EventHandleValid(event)) return kStatusInvalidHandle;
  Open* file = Find(handle);
  if (!file) return kStatusInvalidHandle;
  if (file->directory) return kStatusInvalidParameter;
  if (file->mount->read_only) return kStatusAccessDenied;
  uint64_t offset = offset_ptr ? g.Load64(offset_ptr) : ~0ull;
  if (offset >= 0xFFFFFFFFFFFFFFFEull) offset = file->position;
  std::fstream stream(file->host, std::ios::binary | std::ios::in | std::ios::out);
  stream.seekp(std::streamoff(offset));
  uint32_t done = 0;
  for (uint32_t page = 0; done < length && stream; ++page) {
    const uint32_t n = std::min<uint32_t>(0x1000, length - done);
    stream.write(reinterpret_cast<const char*>(g.host(g.Load32(segments + 4 * page))), n);
    if (stream) done += n;
  }
  const uint32_t status = done == length ? kStatusSuccess : kStatusAccessDenied;
  file->position = offset + done;
  StoreStatus(g, io_status, status, done);
  if (event) Objects().NtSetEvent(g, event, 0);
  return status;
}

uint32_t FileSystem::WriteFile(Guest g, uint32_t handle, uint32_t event, uint32_t io_status, uint32_t buffer, uint32_t length,
                               uint32_t offset_ptr) {
  return Transfer(g, handle, event, io_status, buffer, length, offset_ptr, true);
}

uint32_t FileSystem::DeviceControl(Guest g, uint32_t code, uint32_t output, uint32_t output_length, uint32_t& written) {
  constexpr uint32_t kGetDriveGeometry = 0x70000, kGetPartitionInfo = 0x74004, kCacheBytes = 0xFF000;
  constexpr uint32_t kStatusBufferTooSmall = 0xC0000023;
  written = 0;
  if (code == kGetDriveGeometry) {
    if (output_length < 8) return kStatusBufferTooSmall;
    g.Store32(output, kCacheBytes / 512);  // sectors
    g.Store32(output + 4, 512);            // bytes a sector
    written = 8;
  } else if (code == kGetPartitionInfo) {
    if (output_length < 16) return kStatusBufferTooSmall;
    g.Store64(output, 0);                // starting offset
    g.Store64(output + 8, kCacheBytes);  // length
    written = 16;
  } else {
    return kStatusInvalidParameter;
  }
  return kStatusSuccess;
}

uint32_t FileSystem::DeviceIoControl(Guest g, uint32_t io_status, uint32_t code, uint32_t output, uint32_t output_length) {
  uint32_t written;
  const uint32_t status = DeviceControl(g, code, output, output_length, written);
  if (status == kStatusSuccess && io_status) {
    g.Store32(io_status, kStatusSuccess);
    g.Store32(io_status + 4, written);
  }
  return status;
}

uint32_t FileSystem::SynchronousDeviceIoControl(Guest g, uint32_t code, uint32_t output, uint32_t output_length, uint32_t returned) {
  uint32_t written;
  const uint32_t status = DeviceControl(g, code, output, output_length, written);
  if (status == kStatusSuccess && returned) g.Store32(returned, written);
  return status;
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
    case kFileCompletionInformation: minimum = 8; break;
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
    case kFileCompletionInformation: {
      // The port's handle and the key of the file's packets.
      const uint32_t port = Objects().Lookup(g.Load32(info));
      if (!port || !Objects().IsCompletionPort(g, port)) {
        status = kStatusInvalidHandle;
      } else {
        Objects().ReferenceObject(port);
        if (file->port) Objects().DereferenceObject(g, file->port);
        file->port = port;
        file->key = g.Load32(info + 4);
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
  link_objects_.erase(object);
  auto it = open_.find(object);
  if (it == open_.end()) return;
  if (it->second.delete_on_close) {
    std::error_code error;
    std::filesystem::remove(it->second.host, error);
  }
  const uint32_t port = it->second.port;
  open_.erase(it);
  if (port) Objects().DereferenceObject(Objects().guest(), port);
}

// --- the Win32 file calls XAM exports, on the same open files ---------------------------------------------------

namespace {

constexpr uint32_t kInvalid = 0xFFFFFFFF;  // INVALID_HANDLE_VALUE, INVALID_FILE_ATTRIBUTES, INVALID_SET_FILE_POINTER

std::string CString(Guest g, uint32_t string) {
  return string ? std::string(reinterpret_cast<const char*>(g.host(string))) : std::string();
}

// A FILETIME as the console stores it: low word, then high word, each big-endian.
void StoreFileTime(Guest g, uint32_t at, uint64_t time) {
  g.Store32(at, uint32_t(time));
  g.Store32(at + 4, uint32_t(time >> 32));
}

// The attributes, three times and size of WIN32_FILE_ATTRIBUTE_DATA, the start of WIN32_FIND_DATA too (36 bytes).
void StoreAttributeData(Guest g, uint32_t at, const Stat& s) {
  g.Store32(at, s.attributes);
  for (uint32_t i = 0; i < 3; ++i) StoreFileTime(g, at + 4 + 8 * i, s.time);
  g.Store32(at + 28, uint32_t(s.size >> 32));
  g.Store32(at + 32, uint32_t(s.size));
}

}  // namespace

uint32_t FileSystem::Win32CreateFile(Guest g, uint32_t name, uint32_t access, uint32_t disposition) {
  // CREATE_NEW, CREATE_ALWAYS, OPEN_EXISTING, OPEN_ALWAYS, TRUNCATE_EXISTING; anything else opens.
  static constexpr uint32_t kDispositions[] = {kOpen, kCreate, kOverwriteIf, kOpen, kOpenIf, kOverwrite};
  uint32_t handle = kInvalid, action = 0;
  OpenPath(g, CString(g, name), access, disposition < 6 ? kDispositions[disposition] : kOpen, 0, handle, action);
  return handle;
}

uint32_t FileSystem::Win32Transfer(Guest g, uint32_t handle, uint32_t buffer, uint32_t length, uint32_t done_out, uint32_t overlapped,
                                   bool write) {
  Open* file = Find(handle);
  if (!file || file->directory) {
    if (done_out && !overlapped) g.Store32(done_out, 0);
    return 0;
  }
  // With an OVERLAPPED the transfer happens at its offset (+8 low, +12 high) at once; its status and count are set.
  uint64_t offset = overlapped ? uint64_t(g.Load32(overlapped + 12)) << 32 | g.Load32(overlapped + 8) : file->position;
  uint64_t start = offset;
  uint32_t status = Transfer(g, handle, 0, 0, buffer, length, 0, write, &start);
  const uint32_t done = status == kStatusSuccess ? uint32_t(file->position - offset) : 0;
  if (status == kStatusEndOfFile) status = kStatusSuccess;  // a read at the end is a success of no bytes
  if (overlapped) {
    g.Store32(overlapped, status);
    g.Store32(overlapped + 4, done);
  } else if (done_out) {
    g.Store32(done_out, done);
  }
  return status == kStatusSuccess ? 1 : 0;
}

uint32_t FileSystem::Win32SetFilePointer(Guest g, uint32_t handle, uint32_t low, uint32_t high_ptr, uint32_t method) {
  Open* file = Find(handle);
  if (!file) return kInvalid;
  int64_t distance = int32_t(low);
  if (high_ptr) distance = int64_t(uint64_t(g.Load32(high_ptr)) << 32 | low);
  std::error_code error;
  const uint64_t size = file->directory ? 0 : std::filesystem::file_size(file->host, error);
  uint64_t position;
  switch (method) {
    case 0: position = uint64_t(distance); break;
    case 1: position = file->position + uint64_t(distance); break;
    case 2: position = size + uint64_t(distance); break;
    default: return kInvalid;
  }
  file->position = position;
  if (high_ptr) g.Store32(high_ptr, uint32_t(position >> 32));
  return uint32_t(position);
}

uint32_t FileSystem::Win32SetFilePointerEx(Guest g, uint32_t handle, uint32_t high, uint32_t low, uint32_t position_out, uint32_t method) {
  Open* file = Find(handle);
  if (!file) return 0;
  const uint64_t distance = uint64_t(high) << 32 | low;
  std::error_code error;
  const uint64_t size = file->directory ? 0 : std::filesystem::file_size(file->host, error);
  uint64_t position;
  switch (method) {
    case 0: position = distance; break;
    case 1: position = file->position + distance; break;
    case 2: position = size + distance; break;
    default: return 0;
  }
  file->position = position;
  if (position_out) {
    // Low word first, as the reference writes it.
    g.Store32(position_out, uint32_t(position));
    g.Store32(position_out + 4, uint32_t(position >> 32));
  }
  return 1;
}

uint32_t FileSystem::Win32GetFileSize(Guest g, uint32_t handle, uint32_t high_out) {
  Open* file = Find(handle);
  if (!file) return kInvalid;
  std::error_code error;
  const uint64_t size = file->directory ? 0 : std::filesystem::file_size(file->host, error);
  if (high_out) g.Store32(high_out, uint32_t(size >> 32));
  return uint32_t(size);
}

uint32_t FileSystem::Win32GetFileSizeEx(Guest g, uint32_t handle, uint32_t size_out) {
  Open* file = Find(handle);
  if (!file) return 0;
  std::error_code error;
  const uint64_t size = file->directory ? 0 : std::filesystem::file_size(file->host, error);
  if (size_out) g.Store64(size_out, size);
  return 1;
}

uint32_t FileSystem::Win32SetEndOfFile(uint32_t handle) {
  Open* file = Find(handle);
  if (!file || file->directory || file->mount->read_only) return 0;
  std::error_code error;
  std::filesystem::resize_file(file->host, file->position, error);
  return error ? 0 : 1;
}

uint32_t FileSystem::Win32FlushFileBuffers(uint32_t handle) { return Find(handle) ? 1 : 0; }

uint32_t FileSystem::Win32DeleteFile(Guest g, uint32_t name) {
  std::string rest;
  const std::string guest = CString(g, name);
  const Mounted* mount = MountOf(Canonical(guest), rest);
  const std::filesystem::path host = Resolve(guest);
  std::error_code error;
  if (!mount || mount->read_only || host.empty() || !std::filesystem::is_regular_file(host, error)) return 0;
  return std::filesystem::remove(host, error) ? 1 : 0;
}

uint32_t FileSystem::Win32CreateDirectory(Guest g, uint32_t name) {
  std::string rest;
  const std::string guest = CString(g, name);
  const Mounted* mount = MountOf(Canonical(guest), rest);
  bool parent = false;
  const std::filesystem::path host = Resolve(guest, &parent);
  std::error_code error;
  if (!mount || mount->read_only || !parent || std::filesystem::exists(host, error)) return 0;
  return std::filesystem::create_directory(host, error) ? 1 : 0;
}

uint32_t FileSystem::Win32MoveFile(Guest g, uint32_t from, uint32_t to) {
  std::string rest;
  const std::string source = CString(g, from), target = CString(g, to);
  const Mounted* mount = MountOf(Canonical(source), rest);
  bool parent = false;
  const std::filesystem::path from_host = Resolve(source), to_host = Resolve(target, &parent);
  std::error_code error;
  if (!mount || mount->read_only || MountOf(Canonical(target), rest) != mount || from_host.empty() ||
      !std::filesystem::exists(from_host, error) || !parent || std::filesystem::exists(to_host, error)) {
    return 0;  // MoveFile never replaces an existing file
  }
  std::filesystem::rename(from_host, to_host, error);
  return error ? 0 : 1;
}

uint32_t FileSystem::Win32CopyFile(Guest g, uint32_t from, uint32_t to, uint32_t fail_if_exists) {
  std::string rest;
  const std::string source = CString(g, from), target = CString(g, to);
  const Mounted* mount = MountOf(Canonical(target), rest);
  bool parent = false;
  const std::filesystem::path from_host = Resolve(source), to_host = Resolve(target, &parent);
  std::error_code error;
  if (from_host.empty() || !std::filesystem::is_regular_file(from_host, error) || !mount || mount->read_only || !parent) return 0;
  const auto options = fail_if_exists ? std::filesystem::copy_options::none : std::filesystem::copy_options::overwrite_existing;
  return std::filesystem::copy_file(from_host, to_host, options, error) ? 1 : 0;
}

uint32_t FileSystem::Win32GetFileAttributes(Guest g, uint32_t name) {
  const std::filesystem::path host = Resolve(CString(g, name));
  std::error_code error;
  if (host.empty() || !std::filesystem::exists(host, error)) return kInvalid;
  return StatOf(host).attributes;
}

uint32_t FileSystem::Win32GetFileAttributesEx(Guest g, uint32_t name, uint32_t info) {
  const std::filesystem::path host = Resolve(CString(g, name));
  std::error_code error;
  if (host.empty() || !std::filesystem::exists(host, error)) return 0;
  StoreAttributeData(g, info, StatOf(host));
  return 1;
}

uint32_t FileSystem::Win32GetDiskFreeSpace(Guest g, uint32_t name, uint32_t free_to_caller, uint32_t total, uint32_t free) {
  std::string rest;
  const std::string guest = CString(g, name);
  const Mounted* mount = MountOf(Canonical(guest), rest);
  const std::filesystem::path host = Resolve(guest);
  std::error_code error;
  if (!mount || host.empty() || !std::filesystem::exists(host, error)) return 0;
  const auto space = std::filesystem::space(mount->host, error);
  const uint64_t capacity = error ? 0 : space.capacity, available = error || mount->read_only ? 0 : space.available;
  // Low word first, as the reference writes these.
  for (const auto& [at, value] : {std::pair{free_to_caller, available}, std::pair{total, capacity}, std::pair{free, available}}) {
    if (!at) continue;
    g.Store32(at, uint32_t(value));
    g.Store32(at + 4, uint32_t(value >> 32));
  }
  return 1;
}

uint32_t FileSystem::Win32FindFirstFile(Guest g, uint32_t name, uint32_t data) {
  const std::string path = CString(g, name);
  const size_t slash = path.find_last_of('\\');
  const std::string directory = slash == std::string::npos ? std::string() : path.substr(0, slash);
  const std::string pattern = slash == std::string::npos ? path : path.substr(slash + 1);
  uint32_t handle = kInvalid, action = 0;
  if (OpenPath(g, directory, 0x1, kOpen, kFileDirectoryFile, handle, action) != kStatusSuccess) return kInvalid;
  Open* file = Find(handle);
  file->pattern = pattern.empty() ? "*" : pattern;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(file->host, error)) {
    const std::string entry_name = entry.path().filename().string();
    if (Match(file->pattern, entry_name)) file->listing.push_back(entry_name);
  }
  std::sort(file->listing.begin(), file->listing.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
  if (!Win32FindNextFile(g, handle, data)) {
    Objects().Close(handle);
    return kInvalid;
  }
  return handle;
}

uint32_t FileSystem::Win32FindNextFile(Guest g, uint32_t handle, uint32_t data) {
  Open* file = Find(handle);
  if (!file || !file->directory || file->next >= file->listing.size()) return 0;
  const std::string& entry = file->listing[file->next++];
  // WIN32_FIND_DATA (0x140 bytes): the attribute data, two reserved words, the name (260), the short name (14).
  std::memset(g.host(data), 0, 0x140);
  StoreAttributeData(g, data, StatOf(file->host / entry));
  std::memcpy(g.host(data + 0x2C), entry.data(), std::min<size_t>(entry.size(), 259));
  return 1;
}

void FileSystem::Completed(Guest g, uint32_t handle, uint32_t apc_context, uint32_t status, uint32_t io_status) {
  Open* file = Find(handle);
  if (!file || !file->port) return;
  Objects().QueueCompletion(file->port, file->key, apc_context, status, io_status ? g.Load32(io_status + 4) : 0);
}

uint32_t FileSystem::OpenSymbolicLink(Guest g, uint32_t handle_out, uint32_t attributes) {
  if (!attributes) return kStatusInvalidParameter;
  std::string name = PathOf(g, attributes);
  for (char c : name) {
    if (uint8_t(c) < 32 || uint8_t(c) >= 127) return kStatusObjectNameInvalid;
  }
  name = Lower(name);
  if (name.rfind("\\??\\", 0) == 0) name = name.substr(4);
  const auto it = links_.find(name);
  if (it == links_.end()) return kStatusNoSuchFile;
  const uint32_t object = Objects().AllocateObject(16, true);
  if (!object) return 0xC0000017;
  std::memset(g.host(object), 0, 16);
  link_objects_[object] = it->second;
  const uint32_t handle = Objects().OpenHandle(object);
  if (handle_out) g.Store32(handle_out, handle);
  return kStatusSuccess;
}

uint32_t FileSystem::QuerySymbolicLink(Guest g, uint32_t handle, uint32_t target) {
  const auto it = link_objects_.find(Objects().Lookup(handle));
  if (it == link_objects_.end()) return kStatusNoSuchFile;
  const uint16_t length = uint16_t(std::min<size_t>(g.Load16(target + 2), it->second.size()));
  if (length) std::memcpy(g.host(g.Load32(target + 4)), it->second.data(), length);
  g.Store16(target, length);
  return kStatusSuccess;
}

}  // namespace any360::sys
