// The file system: guest devices mounted on host directories, the kernel's symbolic links ("game:", "\??\D:"), and
// the Nt* file calls on top. Guest paths are case-insensitive and resolved component by component against the host,
// so a title's data works the same on case-sensitive hosts. A device mounted read-only refuses write access.
// Open files are objects of the object manager (handles from the same table, closed by NtClose).
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "guest.h"

namespace any360::sys {

// Statuses of the file calls.
constexpr uint32_t kStatusNoMoreFiles = 0x80000006, kStatusInvalidInfoClass = 0xC0000003,
                   kStatusInfoLengthMismatch = 0xC0000004, kStatusNoSuchFile = 0xC000000F, kStatusEndOfFile = 0xC0000011,
                   kStatusObjectNameInvalid = 0xC0000033, kStatusObjectNameNotFound = 0xC0000034,
                   kStatusObjectNameCollision = 0xC0000035, kStatusObjectPathNotFound = 0xC000003A,
                   kStatusFileIsADirectory = 0xC00000BA, kStatusNotADirectory = 0xC0000103,
                   kStatusDirectoryNotEmpty = 0xC0000101;
// Creation dispositions, the action reported in the I/O status block, create options, attributes.
enum Disposition : uint32_t { kSupersede = 0, kOpen = 1, kCreate = 2, kOpenIf = 3, kOverwrite = 4, kOverwriteIf = 5 };
enum Action : uint32_t { kSuperseded = 0, kOpened = 1, kCreated = 2, kOverwritten = 3, kExists = 4, kDoesNotExist = 5 };
constexpr uint32_t kFileDirectoryFile = 0x1, kFileNonDirectoryFile = 0x40, kFileDeleteOnClose = 0x1000;
constexpr uint32_t kAttributeReadOnly = 0x1, kAttributeDirectory = 0x10, kAttributeNormal = 0x80;
// Information classes.
enum FileInfo : uint32_t {
  kFileDirectoryInformation = 1, kFileBasicInformation = 4, kFileStandardInformation = 5, kFileInternalInformation = 6,
  kFileRenameInformation = 10, kFileDispositionInformation = 13, kFilePositionInformation = 14, kFileModeInformation = 16,
  kFileAlignmentInformation = 17, kFileAllocationInformation = 19, kFileEndOfFileInformation = 20,
  kFileNetworkOpenInformation = 34
};
enum VolumeInfo : uint32_t { kFsVolumeInformation = 1, kFsSizeInformation = 3, kFsDeviceInformation = 4, kFsAttributeInformation = 5 };

class FileSystem {
 public:
  FileSystem();
  void Reset();
  // `device` is a guest device path ("\Device\Cdrom0", "\Device\Harddisk0\Partition1").
  void Mount(const std::string& device, const std::filesystem::path& host, bool read_only);
  // A symbolic link from a DOS name ("game:", "\??\D:") to a device path.
  void Link(const std::string& link, const std::string& target);
  void Unmount(const std::string& device);
  void Unlink(const std::string& link);

  // ObCreateSymbolicLink / ObDeleteSymbolicLink: ANSI_STRING pointers.
  uint32_t CreateSymbolicLink(Guest g, uint32_t link, uint32_t target);
  uint32_t DeleteSymbolicLink(Guest g, uint32_t link);

  uint32_t CreateFile(Guest g, uint32_t handle_out, uint32_t access, uint32_t attributes, uint32_t io_status, uint32_t file_attributes,
                      uint32_t disposition, uint32_t options);
  uint32_t ReadFile(Guest g, uint32_t handle, uint32_t event, uint32_t io_status, uint32_t buffer, uint32_t length, uint32_t offset_ptr);
  // NtReadFileScatter: `length` bytes into the 4 KB pages listed (one 32-bit pointer each) at `segments`.
  uint32_t ReadFileScatter(Guest g, uint32_t handle, uint32_t event, uint32_t io_status, uint32_t segments, uint32_t length,
                           uint32_t offset_ptr);
  uint32_t WriteFile(Guest g, uint32_t handle, uint32_t event, uint32_t io_status, uint32_t buffer, uint32_t length, uint32_t offset_ptr);
  // NtWriteFileGather: `length` bytes from the 4 KB pages listed (one 32-bit pointer each) at `segments`.
  uint32_t WriteFileGather(Guest g, uint32_t handle, uint32_t event, uint32_t io_status, uint32_t segments, uint32_t length,
                           uint32_t offset_ptr);
  uint32_t QueryInformation(Guest g, uint32_t handle, uint32_t io_status, uint32_t info, uint32_t length, uint32_t info_class);
  // NtDeviceIoControlFile: the disk queries the cache (utility) partition's mount makes, answered for a 1 MB
  // partition of 512-byte sectors as the reference does (drive geometry, partition info); others are invalid.
  uint32_t DeviceIoControl(Guest g, uint32_t io_status, uint32_t code, uint32_t output, uint32_t output_length);
  // IoSynchronousDeviceIoControlRequest: the same queries, the bytes written stored at `returned` when given.
  uint32_t SynchronousDeviceIoControl(Guest g, uint32_t code, uint32_t output, uint32_t output_length, uint32_t returned);
  uint32_t SetInformation(Guest g, uint32_t handle, uint32_t io_status, uint32_t info, uint32_t length, uint32_t info_class);
  uint32_t QueryFullAttributes(Guest g, uint32_t attributes, uint32_t info);
  uint32_t QueryDirectory(Guest g, uint32_t handle, uint32_t io_status, uint32_t info, uint32_t length, uint32_t pattern, uint32_t restart);
  uint32_t QueryVolume(Guest g, uint32_t handle, uint32_t io_status, uint32_t info, uint32_t length, uint32_t info_class);
  uint32_t Flush(Guest g, uint32_t handle, uint32_t io_status);

  // The Win32 file calls XAM exports (CreateFileA, ReadFile...), on the same open files and handles. They answer as
  // Win32 does (a handle or INVALID_HANDLE_VALUE, TRUE or FALSE) without setting the last error, as the reference.
  // Reads and writes with an OVERLAPPED complete at once at its offset, its status and count set.
  uint32_t Win32CreateFile(Guest g, uint32_t name, uint32_t access, uint32_t disposition);
  uint32_t Win32Transfer(Guest g, uint32_t handle, uint32_t buffer, uint32_t length, uint32_t done_out, uint32_t overlapped, bool write);
  uint32_t Win32SetFilePointer(Guest g, uint32_t handle, uint32_t low, uint32_t high_ptr, uint32_t method);
  // The distance comes as two words (high, low) and the new position is stored low word first, as the reference does.
  uint32_t Win32SetFilePointerEx(Guest g, uint32_t handle, uint32_t high, uint32_t low, uint32_t position_out, uint32_t method);
  uint32_t Win32GetFileSize(Guest g, uint32_t handle, uint32_t high_out);
  uint32_t Win32GetFileSizeEx(Guest g, uint32_t handle, uint32_t size_out);
  uint32_t Win32SetEndOfFile(uint32_t handle);
  uint32_t Win32FlushFileBuffers(uint32_t handle);
  uint32_t Win32DeleteFile(Guest g, uint32_t name);
  uint32_t Win32CreateDirectory(Guest g, uint32_t name);
  uint32_t Win32MoveFile(Guest g, uint32_t from, uint32_t to);
  uint32_t Win32CopyFile(Guest g, uint32_t from, uint32_t to, uint32_t fail_if_exists);
  uint32_t Win32GetFileAttributes(Guest g, uint32_t name);
  uint32_t Win32GetFileAttributesEx(Guest g, uint32_t name, uint32_t info);
  // Free and total bytes of the drive holding a path (its host volume; none free on a read-only device), low word first.
  uint32_t Win32GetDiskFreeSpace(Guest g, uint32_t name, uint32_t free_to_caller, uint32_t total, uint32_t free);
  // A search: "<directory>\<pattern>", the matches sorted; the handle is a directory handle, closed by CloseHandle.
  uint32_t Win32FindFirstFile(Guest g, uint32_t name, uint32_t data);
  uint32_t Win32FindNextFile(Guest g, uint32_t handle, uint32_t data);

  // The host path a guest path resolves to; empty when the device or a parent directory does not exist.
  std::filesystem::path Resolve(const std::string& guest_path, bool* parent_exists = nullptr) const;
  // Called by the object manager when the last handle of an object closes.
  void OnClose(uint32_t object);

 private:
  struct Mounted {
    std::string device;  // lower case
    std::filesystem::path host;
    bool read_only;
  };
  struct Open {
    std::filesystem::path host;
    std::string guest;
    const Mounted* mount = nullptr;
    bool directory = false, delete_on_close = false;
    uint64_t position = 0;
    std::vector<std::string> listing;  // directory entries, sorted, for the current search
    std::string pattern;
    size_t next = 0;
  };
  Open* Find(uint32_t handle);
  const Mounted* MountOf(const std::string& device_path, std::string& rest) const;
  std::string Canonical(const std::string& guest_path) const;
  std::string PathOf(Guest g, uint32_t attributes) const;
  uint32_t DeviceControl(Guest g, uint32_t code, uint32_t output, uint32_t output_length, uint32_t& written);
  // `at`, when given, is the offset to use instead of `offset_ptr` or the file position.
  uint32_t Transfer(Guest g, uint32_t handle, uint32_t event, uint32_t io_status, uint32_t buffer, uint32_t length, uint32_t offset_ptr,
                    bool write, const uint64_t* at = nullptr);
  // Opens a guest path as NtCreateFile does: the handle (INVALID_HANDLE_VALUE on failure) and the action taken.
  uint32_t OpenPath(Guest g, const std::string& guest, uint32_t access, uint32_t disposition, uint32_t options, uint32_t& handle,
                    uint32_t& action);

  std::vector<Mounted> mounts_;
  std::map<std::string, std::string> links_;  // lower-case DOS name -> device path
  std::map<uint32_t, Open> open_;             // object -> open file
};

FileSystem& Files();

}  // namespace any360::sys
