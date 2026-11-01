// xam.xex and the process-level kernel calls a title needs to boot and run offline: XAM messages and notifications,
// the signed-in local user and its profile settings, sessions, enumerators, content packages (saves) over the file
// system, the controllers (through an input source the host provides), the title's XEX module, the network as a
// console without a connection, and the process exits (bug checks, terminate, return to firmware) through hooks.
// The answers are those of a retail console with one local profile signed in and no Xbox Live.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "guest.h"

namespace any360::sys {

// Win32 / XAM result codes.
constexpr uint32_t kErrorSuccess = 0, kErrorNoMoreFiles = 0x12, kErrorInvalidParameter = 0x57, kErrorInsufficientBuffer = 0x7A,
                   kErrorBadArguments = 0xA0, kErrorIoPending = 0x3E5, kErrorDeviceNotConnected = 0x48F, kErrorNotFound = 0x490,
                   kErrorNoSuchUser = 0x525, kErrorEmpty = 0x10D2, kErrorAlreadyExists = 0xB7, kErrorFileNotFound = 0x2;
// Notification ids.
constexpr uint32_t kNotifySigninChanged = 0x0000000A, kNotifySystemUi = 0x00000009;

struct GamepadState {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t thumb_lx = 0, thumb_ly = 0, thumb_rx = 0, thumb_ry = 0;
};

// What the host provides: controllers, and what happens when the title stops.
struct XamHost {
  bool (*controller_connected)(uint32_t user) = nullptr;  // default: controller 0 only
  bool (*controller_state)(uint32_t user, GamepadState& state) = nullptr;
  void (*vibrate)(uint32_t user, uint16_t left, uint16_t right) = nullptr;
  // A bug check, an uncaught guest exception, a title terminate or a return to firmware: the default exits the process.
  void (*stop)(const char* reason, uint32_t code) = nullptr;
  // Resolves XexGetProcedureAddress for a system module ("xam.xex", "xboxkrnl.exe") by ordinal; 0 when unknown.
  uint32_t (*procedure)(const char* module, uint32_t ordinal) = nullptr;
};

struct TitleSettings {
  uint32_t module_handle = 0;  // the title's module (its XEX header address doubles as the handle)
  uint32_t xex_header = 0;     // guest address of the XEX2 header, for RtlImageXexHeaderField and friends
  std::string module_name = "default.xex";
  std::filesystem::path content_root;  // where content packages (saves, DLC) live on the host
  uint64_t xuid = 0xE000000000000001ull;  // the local profile (an offline XUID)
  std::string gamertag = "Player";
};

class Xam {
 public:
  Xam();
  void Reset();
  XamHost& host() { return host_; }
  TitleSettings& title() { return title_; }

  // Overlapped completion as the kernel does it: result, length and extended error written, the event set.
  void Complete(Guest g, uint32_t overlapped, uint32_t result, uint32_t length = 0);
  // A result returned directly, or through the overlapped structure (then ERROR_IO_PENDING).
  uint32_t Finish(Guest g, uint32_t overlapped, uint32_t result, uint32_t length = 0);
  void Notify(uint32_t id, uint32_t param);

  // Messages and notifications.
  uint32_t MsgStartIORequest(Guest g, uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer, uint32_t length);
  uint32_t MsgInProcessCall(uint32_t app, uint32_t message, uint32_t arg1, uint32_t arg2);
  uint32_t MsgCancelIORequest(Guest g, uint32_t overlapped, uint32_t wait);
  uint32_t NotifyCreateListener(Guest g, uint64_t mask, uint32_t max_version);
  uint32_t NotifyGetNext(Guest g, uint32_t handle, uint32_t match_id, uint32_t id_ptr, uint32_t param_ptr);

  // The local user and profile.
  uint32_t UserGetSigninState(uint32_t user);
  uint32_t UserGetXUID(Guest g, uint32_t user, uint32_t type_mask, uint32_t xuid_ptr);
  uint32_t UserGetName(Guest g, uint32_t user, uint32_t buffer, uint32_t length);
  uint32_t UserGetSigninInfo(Guest g, uint32_t user, uint32_t flags, uint32_t info);
  uint32_t UserCheckPrivilege(Guest g, uint32_t user, uint32_t mask, uint32_t out);
  uint32_t UserReadProfileSettings(Guest g, uint32_t title_id, uint32_t user, uint32_t xuid_count, uint32_t xuids,
                                   uint32_t setting_count, uint32_t setting_ids, uint32_t size_ptr, uint32_t buffer, uint32_t overlapped);
  uint32_t UserWriteProfileSettings(Guest g, uint32_t title_id, uint32_t user, uint32_t setting_count, uint32_t settings,
                                    uint32_t overlapped);
  uint32_t SessionCreateHandle(Guest g, uint32_t handle_ptr);
  uint32_t SessionRefObjByHandle(Guest g, uint32_t handle, uint32_t object_ptr);
  uint32_t ShowSigninUI(uint32_t users, uint32_t flags);
  uint32_t ShowMessageBoxUI(Guest g, uint32_t result_ptr, uint32_t overlapped);
  uint32_t ShowDeviceSelectorUI(Guest g, uint32_t device_id_ptr, uint32_t overlapped);

  // Enumerators: a handle on items of `item_size` bytes, handed out `per_call` at a time.
  uint32_t CreateEnumerator(Guest g, uint32_t item_size, uint32_t per_call, std::vector<std::vector<uint8_t>> items);
  uint32_t Enumerate(Guest g, uint32_t handle, uint32_t flags, uint32_t buffer, uint32_t length, uint32_t items_ptr, uint32_t overlapped);
  uint32_t GetPrivateEnumStructure(Guest g, uint32_t handle, uint32_t out);

  // Content packages: folders under content_root/<type>/<file name>, mounted as "<root name>:" while open.
  uint32_t ContentCreate(Guest g, uint32_t user, uint32_t root_name, uint32_t content_data, uint32_t flags, uint32_t disposition_ptr,
                         uint32_t license_ptr, uint32_t overlapped);
  uint32_t ContentClose(Guest g, uint32_t root_name, uint32_t overlapped);
  uint32_t ContentDelete(Guest g, uint32_t user, uint32_t content_data, uint32_t overlapped);
  uint32_t ContentCreateEnumerator(Guest g, uint32_t user, uint32_t device_id, uint32_t content_type, uint32_t flags, uint32_t per_call,
                                   uint32_t size_ptr, uint32_t handle_out);
  uint32_t ContentGetDeviceState(Guest g, uint32_t device_id, uint32_t overlapped);

  // Controllers.
  uint32_t InputGetCapabilities(Guest g, uint32_t user, uint32_t flags, uint32_t caps);
  uint32_t InputGetState(Guest g, uint32_t user, uint32_t flags, uint32_t state);
  uint32_t InputSetState(Guest g, uint32_t user, uint32_t vibration);
  uint32_t InputGetKeystroke(Guest g, uint32_t user, uint32_t flags, uint32_t keystroke);

  // The title module.
  uint32_t ImageXexHeaderField(Guest g, uint32_t header, uint32_t key) const;
  uint32_t GetModuleHandle(Guest g, uint32_t name, uint32_t handle_ptr);
  uint32_t GetProcedureAddress(Guest g, uint32_t module, uint32_t ordinal, uint32_t out);
  uint32_t CheckExecutablePrivilege(Guest g, uint32_t privilege) const;
  uint32_t GetExecutionId(Guest g, uint32_t info_ptr) const;
  void RegisterTitleTerminateNotification(uint32_t registration, uint32_t create);

  // Launch data handed from one title to the next (none until a title sets it).
  uint32_t LoaderSetLaunchData(Guest g, uint32_t data, uint32_t size);
  uint32_t LoaderGetLaunchDataSize(Guest g, uint32_t size_ptr);
  uint32_t LoaderGetLaunchData(Guest g, uint32_t buffer, uint32_t size);
  // A named section of the title's resources (XEX_HEADER_RESOURCE_INFO): its address and size.
  uint32_t GetModuleSection(Guest g, uint32_t module, uint32_t name, uint32_t data_ptr, uint32_t size_ptr);
  // XDEVICE_DATA of the hard drive (1) and the disc (2).
  uint32_t ContentGetDeviceData(Guest g, uint32_t device_id, uint32_t data);
  uint32_t ContentGetCreator(Guest g, uint32_t user, uint32_t content_data, uint32_t is_creator_ptr, uint32_t xuid_ptr, uint32_t overlapped);
  // The file system cache size a title asks for.
  uint32_t SetCacheElementCount(uint32_t count);
  uint32_t GetCacheElementCount() const { return cache_elements_; }
  // The kernel pool: whole pages, so a block is freed by its address alone.
  uint32_t AllocatePool(Guest g, uint32_t size);
  void FreePool(uint32_t address);
  // A device object for a title's own driver: 0x24 bytes and a page of extension at +0x18.
  uint32_t CreateDevice(Guest g, uint32_t device_out);

  // Stops.
  void Stop(const char* reason, uint32_t code);
  uint32_t RaiseException(Guest g, uint32_t record);

  // The network of a console without a connection.
  uint32_t NetStartup();
  uint32_t WsaStartup(Guest g, uint32_t version, uint32_t data);
  uint32_t Socket();
  uint32_t SocketFailure(uint32_t socket);
  uint32_t WsaGetLastError() const;
  uint32_t XNetGetTitleXnAddr(Guest g, uint32_t address);

 private:
  struct Listener {
    uint32_t object;
    uint64_t mask;
    std::vector<std::pair<uint32_t, uint32_t>> queue;
  };
  struct Enumerator {
    uint32_t object, item_size, per_call;
    size_t next = 0;
    std::vector<std::vector<uint8_t>> items;
  };
  struct Setting {
    uint32_t id;
    std::vector<uint8_t> data;  // 16 bytes of X_USER_PROFILE_SETTING_DATA, plus the binary / string bytes
  };
  Listener* FindListener(uint32_t handle);
  Enumerator* FindEnumerator(uint32_t handle);
  std::filesystem::path ContentPath(Guest g, uint32_t content_data) const;

  XamHost host_;
  TitleSettings title_;
  std::vector<Listener> listeners_;
  std::vector<Enumerator> enumerators_;
  std::vector<Setting> settings_;
  std::vector<uint32_t> terminate_notifications_;
  std::vector<std::string> open_content_;
  uint32_t packet_number_ = 0;
  std::vector<uint8_t> launch_data_;
  bool launch_data_present_ = false;
  uint32_t cache_elements_ = 0;
};

Xam& XamState();

}  // namespace any360::sys
