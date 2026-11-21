// The file system: guest devices mounted on host directories, the kernel's symbolic links ("game:", "\??\D:"), and
// the Nt* file calls on top. Guest paths are case-insensitive and resolved component by component against the host,
// so a title's data works the same on case-sensitive hosts. A device mounted read-only refuses write access.
// Open files are objects of the object manager (handles from the same table, closed by NtClose).
#pragma once

#include <functional>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <shared_mutex>
#include <mutex>
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
constexpr uint32_t kFileDirectoryFile = 0x1, kFileNonDirectoryFile = 0x40, kFileDeleteOnClose = 0x1000,
                   kFileSynchronousIo = 0x30;  // FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT
constexpr uint32_t kAttributeReadOnly = 0x1, kAttributeDirectory = 0x10, kAttributeNormal = 0x80;
// Information classes.
enum FileInfo : uint32_t {
  kFileDirectoryInformation = 1, kFileBasicInformation = 4, kFileStandardInformation = 5, kFileInternalInformation = 6,
  kFileRenameInformation = 10, kFileDispositionInformation = 13, kFilePositionInformation = 14, kFileModeInformation = 16,
  kFileAlignmentInformation = 17, kFileAllocationInformation = 19, kFileEndOfFileInformation = 20, kFileCompletionInformation = 30,
  kFileNetworkOpenInformation = 34
};
enum VolumeInfo : uint32_t { kFsVolumeInformation = 1, kFsSizeInformation = 3, kFsDeviceInformation = 4, kFsAttributeInformation = 5 };

class FileSystem {
 public:
  FileSystem();
  void Reset();
  // `device` is a guest device path ("\Device\Cdrom0", "\Device\Harddisk0\Partition1").
  void Mount(std::string device, const std::filesystem::path& host, bool read_only);
  // A raw device titles probe and format but need not keep (the hard drive's Partition0, Cache0, Cache1, as the
  // reference answers them): the device opens, reads and writes succeed and transfer nothing.
  void MountNull(std::string device);
  // A symbolic link from a DOS name ("game:", "\??\D:") to a device path.
  void Link(std::string link, std::string target);
  void Unmount(const std::string& device);
  bool HasLink(const std::string& link) const;  // whether a DOS name ("<name>:") is linked
  // Whether `device` (any case) is mounted, writable, on the folder `root` / `type` / `name` (ASCII parts), compared
  // in place: no host path is built.
  bool IsMountedOn(std::string_view device, const std::filesystem::path& root, std::string_view type,
                   std::string_view name) const;
  void Unlink(const std::string& link);

  // ObCreateSymbolicLink / ObDeleteSymbolicLink: ANSI_STRING pointers.
  uint32_t CreateSymbolicLink(Guest g, uint32_t link, uint32_t target);
  uint32_t DeleteSymbolicLink(Guest g, uint32_t link);
  // NtOpenSymbolicLinkObject: a handle on a link by its name ("\??\D:" or "D:"); STATUS_NO_SUCH_FILE when there is
  // none. NtQuerySymbolicLinkObject: its target into an ANSI_STRING, up to the buffer's maximum length.
  uint32_t OpenSymbolicLink(Guest g, uint32_t handle_out, uint32_t attributes);
  uint32_t QuerySymbolicLink(Guest g, uint32_t handle, uint32_t target);

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

  // After an Nt* read or write on `handle`: a packet (the file's key, the APC context, the status and the count from the
  // I/O status block, 0 without one) on the completion port the file is associated with, if any.
  void Completed(Guest g, uint32_t handle, uint32_t apc_context, uint32_t status, uint32_t io_status);

  // Whether a guest path names a device mounted here (links followed); a binder sends the paths of devices it does not
  // know to another file system. With OBJECT_ATTRIBUTES, a root directory handle must be an open directory here.
  bool Knows(const std::string& guest_path) const;
  bool KnowsAttributes(Guest g, uint32_t attributes) const;

  // The host path a guest path resolves to; empty when the device or a parent directory does not exist.
  std::filesystem::path Resolve(const std::string& guest_path, bool* parent_exists = nullptr) const;
  // Called by the object manager when the last handle of an object closes.
  bool OnClose(uint32_t object);  // whether it held state for the object (a link or an open file)

 private:
  struct Mounted {
    std::string device;  // lower case
    std::filesystem::path host;
    bool read_only;
    bool null = false;  // a raw device whose opens, reads and writes succeed and move nothing (MountNull)
  };
  // A mount entry is shared, never changed once listed: an open file keeps the one it was opened on, so a Mount,
  // a remount of the same device or an Unmount while it is open leaves it valid (the list itself is reordered).
  using MountRef = std::shared_ptr<const Mounted>;
  // What an open handle keeps of its host file between calls (files.cpp).
  struct HostFile;
  struct Open {
    std::filesystem::path host;
    std::string guest;
    std::shared_ptr<HostFile> host_file;  // none for a raw device
    MountRef mount;
    bool directory = false, delete_on_close = false;
    // Opened without FILE_SYNCHRONOUS_IO_*: NtReadFile / NtWriteFile complete at once but answer STATUS_PENDING, as
    // the reference does, so kernel32's overlapped ReadFile takes its pending path (Crackdown's save loader reads
    // the length back through GetOverlappedResult only on that path).
    bool asynchronous = false;
    uint32_t port = 0, key = 0;  // the completion port (referenced) and the key of its packets
    uint64_t position = 0;
    std::vector<std::string> listing;  // directory entries, sorted, for the current search
    std::string pattern;
    size_t next = 0;
  };
  Open* Find(uint32_t handle);
  // A transfer's answer to NtReadFile / NtWriteFile: STATUS_PENDING for a completed one on an asynchronous file.
  uint32_t Pending(uint32_t handle, uint32_t status);
  MountRef MountOf(const std::string& device_path, std::string& rest) const;
  void AddMount(std::string device, const std::filesystem::path& host, bool read_only, bool null);
  std::string Canonical(const std::string& guest_path) const;
  std::string PathOf(Guest g, uint32_t attributes) const;
  uint32_t DeviceControl(Guest g, uint32_t code, uint32_t output, uint32_t output_length, uint32_t& written);
  // The file layer is about to resize, rename, remove or create host files: every handle lets go of the host file
  // it keeps open for its reads, which the host needs before it renames or removes one.
  void HostChanged();
  // Up to `length` bytes of a handle's host file from `offset`; `usable` is false when the host file cannot be
  // opened. The handle keeps the host file open for its next reads, another giving its own up where the handles
  // already keep as many as they may.
  uint32_t ReadHost(Open& file, uint64_t offset, uint8_t* to, uint32_t length, bool& usable);

 public:
  // Whether a handle keeps its host file open between its reads. Off: every read opens the host file, as before
  // the handles kept it; for telling whether a title's trouble comes from that.
  void SetKeepHostFiles(bool keep) { keep_host_files_ = keep; }
  // Where the layer says what goes wrong on the host (a host file that does not open for a read: the title gets
  // nothing and no error) and how many files the title holds open at once. With `calls`, also every open and close
  // and every read that gives fewer bytes than asked, by guest path: what a title did with its files before it
  // went wrong.
  void SetLog(std::function<void(const std::string&)> log, bool calls = false) {
    log_ = std::move(log);
    log_calls_ = calls;
  }

 private:
  bool keep_host_files_ = true;
  std::function<void(const std::string&)> log_;
  bool log_calls_ = false;
  std::atomic<uint32_t> log_peak_{128};     // the next count of open files worth a line
  std::atomic<uint32_t> log_unopened_{0};   // lines written about host files that do not open
  std::atomic<bool> log_gave_up_{false};    // the line about handles giving their host files up is written
  void LogOpen(const std::string& guest, uint32_t status, uint32_t handle, size_t open_now);
  void LogRead(const Open& file, uint64_t offset, uint32_t length, uint32_t done, bool usable);
  // `at`, when given, is the offset to use instead of `offset_ptr` or the file position.
  uint32_t Transfer(Guest g, uint32_t handle, uint32_t event, uint32_t io_status, uint32_t buffer, uint32_t length, uint32_t offset_ptr,
                    bool write, const uint64_t* at = nullptr);
  // Opens a guest path as NtCreateFile does: the handle (INVALID_HANDLE_VALUE on failure) and the action taken.
  uint32_t OpenPath(Guest g, const std::string& guest, uint32_t access, uint32_t disposition, uint32_t options, uint32_t& handle,
                    uint32_t& action);

  std::vector<MountRef> mounts_;       // longest device path first
  mutable std::shared_mutex mounts_mutex_;  // the list: titles mount content on one thread while others open files
  std::map<std::string, std::string> links_;  // lower-case DOS name -> device path
  std::map<uint32_t, Open> open_;             // object -> open file
  // The table itself (a look-up, an insertion, a removal, a walk): titles open and close files on several threads
  // at once. An entry is its handle's, used without the lock by the calls on that handle. Never held across a call
  // into the object manager, which calls OnClose under its own lock.
  mutable std::mutex open_mutex_;
  std::map<uint32_t, std::string> link_objects_;  // object -> the target of the link it was opened on
  // links_ and link_objects_: a title links content (XamContentCreate) on one thread while others resolve paths.
  // Never held across a call into the object manager.
  mutable std::shared_mutex links_mutex_;
};

FileSystem& Files();

}  // namespace any360::sys
