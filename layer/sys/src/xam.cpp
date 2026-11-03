#include "any360/sys/xam.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>

#include "any360/sys/files.h"
#include "any360/sys/memory.h"
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"

namespace any360::sys {

namespace {

constexpr uint32_t kStatusSuccess = 0, kStatusNotFound = 0xC0000225, kStatusProcedureNotFound = 0xC000007A;
constexpr uint32_t kXuserSize = 40;                    // X_USER_PROFILE_SETTING
constexpr uint32_t kContentDataSize = 0x134;           // XCONTENT_DATA: device, type, 128 wide chars, 42-byte file name
constexpr uint32_t kContentFileNameOffset = 0x108;
constexpr uint32_t kWsaENetDown = 10050, kWsaENotSock = 10038, kWsaInvalidHandle = 6, kWsaInvalidParameter = 87;
constexpr uint32_t kWsaWaitTimeout = 0x102, kWsaWaitFailed = 0xFFFFFFFF;
constexpr uint32_t kInputFlagGamepad = 1;

thread_local uint32_t t_wsa_error = 0;

std::string GuestString(Guest g, uint32_t address, size_t max = 256) {
  if (!address) return {};
  std::string s;
  for (size_t i = 0; i < max; ++i) {
    const char c = char(g.Load8(address + uint32_t(i)));
    if (!c) break;
    s += c;
  }
  return s;
}

void DefaultStop(const char* reason, uint32_t code) {
  std::fprintf(stderr, "any360::sys: %s (0x%08X)\n", reason, code);
  std::fflush(stderr);
  std::exit(code == 0 ? 0 : 1);
}

}  // namespace

Xam& XamState() {
  static Xam xam;
  return xam;
}

Xam::Xam() { Reset(); }

void Xam::Reset() {
  host_ = {};
  title_ = {};
  listeners_.clear();
  enumerators_.clear();
  system_module_ = 0;
  settings_.clear();
  terminate_notifications_.clear();
  open_content_.clear();
  packet_number_ = 0;
  launch_data_.clear();
  launch_data_present_ = false;
  cache_elements_ = 0;
}

// --- overlapped ------------------------------------------------------------------------------------------------

void Xam::Complete(Guest g, uint32_t overlapped, uint32_t result, uint32_t length) {
  // XOVERLAPPED: result, length, context, event, completion routine and context, extended error.
  g.Store32(overlapped + 0x00, result);
  g.Store32(overlapped + 0x04, length);
  g.Store32(overlapped + 0x18, result);
  const uint32_t event = g.Load32(overlapped + 0x0C);
  if (event) Objects().NtSetEvent(g, event, 0);
}

uint32_t Xam::Finish(Guest g, uint32_t overlapped, uint32_t result, uint32_t length) {
  if (!overlapped) return result;
  Complete(g, overlapped, result, length);
  return kErrorIoPending;
}

// --- messages and notifications ------------------------------------------------------------------------------------

uint32_t Xam::MsgStartIORequest(Guest g, uint32_t, uint32_t, uint32_t overlapped, uint32_t, uint32_t) {
  // The system applications (presence, achievements, marketplace) accept every request of an offline title.
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::MsgInProcessCall(uint32_t, uint32_t, uint32_t, uint32_t) { return kErrorSuccess; }

uint32_t Xam::MsgCancelIORequest(Guest, uint32_t, uint32_t) { return kErrorSuccess; }

uint32_t Xam::MsgCompleteIORequest(Guest g, uint32_t overlapped, uint32_t result, uint32_t extended_error, uint32_t length) {
  g.Store32(overlapped + 0x00, result);
  g.Store32(overlapped + 0x04, length);
  g.Store32(overlapped + 0x18, extended_error);
  const uint32_t event = g.Load32(overlapped + 0x0C);
  if (event) Objects().NtSetEvent(g, event, 0);
  return kErrorSuccess;
}

uint32_t Xam::GetOverlappedResult(Guest g, uint32_t overlapped, uint32_t length_ptr, uint32_t wait, uint32_t thread) {
  constexpr uint32_t kErrorIoIncomplete = 0x3E4;
  uint32_t result = g.Load32(overlapped);
  if (result == kErrorIoPending) {
    const uint32_t event = g.Load32(overlapped + 0x0C);
    if (!wait || !event) return kErrorIoIncomplete;
    if (Objects().WaitForHandle(g, event, thread, nullptr) != kStatusSuccess) return kErrorInvalidParameter;
    result = g.Load32(overlapped);
  }
  if (result == kErrorSuccess && length_ptr) g.Store32(length_ptr, g.Load32(overlapped + 0x04));
  return result;
}

void Xam::Notify(uint32_t id, uint32_t param) {
  const uint32_t area = (id >> 25) & 0x3F;
  for (Listener& l : listeners_) {
    if (l.mask & (1ull << area)) l.queue.emplace_back(id, param);
  }
}

uint32_t Xam::NotifyCreateListener(Guest g, uint64_t mask, uint32_t) {
  const uint32_t object = Objects().AllocateObject(16);
  if (!object) return 0;
  std::memset(g.host(object), 0, 16);
  Listener l{object, mask, {}};
  // A new listener first hears that the local profile is signed in.
  if (mask & 1) l.queue.emplace_back(kNotifySigninChanged, 1);
  listeners_.push_back(std::move(l));
  return Objects().OpenHandle(object);
}

Xam::Listener* Xam::FindListener(uint32_t handle) {
  const uint32_t object = Objects().Lookup(handle);
  for (Listener& l : listeners_) {
    if (object && l.object == object) return &l;
  }
  return nullptr;
}

uint32_t Xam::NotifyGetNext(Guest g, uint32_t handle, uint32_t match_id, uint32_t id_ptr, uint32_t param_ptr) {
  if (param_ptr) g.Store32(param_ptr, 0);
  if (!id_ptr) return 0;
  g.Store32(id_ptr, 0);
  Listener* l = FindListener(handle);
  if (!l) return 0;
  for (size_t i = 0; i < l->queue.size(); ++i) {
    if (match_id && l->queue[i].first != match_id) continue;
    g.Store32(id_ptr, l->queue[i].first);
    if (param_ptr) g.Store32(param_ptr, l->queue[i].second);
    l->queue.erase(l->queue.begin() + long(i));
    return 1;
  }
  return 0;
}

// --- the local user -------------------------------------------------------------------------------------------------

uint32_t Xam::UserGetSigninState(uint32_t user) { return user == 0 ? 1 : 0; }  // signed in locally

uint32_t Xam::UserGetXUID(Guest g, uint32_t user, uint32_t, uint32_t xuid_ptr) {
  if (!xuid_ptr) return kErrorInvalidParameter;
  if (user != 0) return kErrorNoSuchUser;
  g.Store64(xuid_ptr, title_.xuid);
  return kErrorSuccess;
}

uint32_t Xam::UserGetName(Guest g, uint32_t user, uint32_t buffer, uint32_t length) {
  if (user != 0) return kErrorNoSuchUser;
  if (!buffer || !length) return kErrorInvalidParameter;
  const uint32_t n = std::min<uint32_t>(uint32_t(title_.gamertag.size()), length - 1);
  std::memcpy(g.host(buffer), title_.gamertag.data(), n);
  g.Store8(buffer + n, 0);
  return kErrorSuccess;
}

uint32_t Xam::UserGetSigninInfo(Guest g, uint32_t user, uint32_t, uint32_t info) {
  if (!info) return 0x80070057;  // E_INVALIDARG
  std::memset(g.host(info), 0, 40);
  if (user != 0) return kErrorNoSuchUser;
  // XUSER_SIGNIN_INFO: XUID, flags, state, guest number, sponsor index, 16-byte name.
  g.Store64(info, title_.xuid);
  g.Store32(info + 12, 1);
  std::memcpy(g.host(info + 24), title_.gamertag.data(), std::min<size_t>(15, title_.gamertag.size()));
  return kErrorSuccess;
}

uint32_t Xam::UserCheckPrivilege(Guest g, uint32_t user, uint32_t, uint32_t out) {
  if (user != 0xFF && user != 0) return kErrorNoSuchUser;
  if (out) g.Store32(out, 1);
  return kErrorSuccess;
}

uint32_t Xam::UserReadProfileSettings(Guest g, uint32_t, uint32_t user, uint32_t xuid_count, uint32_t, uint32_t setting_count,
                                      uint32_t setting_ids, uint32_t size_ptr, uint32_t buffer, uint32_t overlapped) {
  // The setting id carries its type (bits 28-31) and the size of its data (bits 16-27).
  if (setting_count < 1 || setting_count > 32 || !size_ptr) return kErrorInvalidParameter;
  const uint32_t size = g.Load32(size_ptr);
  if (size && !buffer) return kErrorInvalidParameter;
  uint32_t data_size = 0;
  for (uint32_t i = 0; i < setting_count; ++i) {
    const uint32_t id = g.Load32(setting_ids + 4 * i), type = id >> 28;
    if (type == 4 || type == 6) data_size += (id >> 16) & 0xFFF;  // wide string, binary
  }
  const uint32_t copies = xuid_count ? xuid_count : 1;
  const uint32_t needed = 8 + kXuserSize * setting_count * copies + data_size * copies;
  if (!buffer || size < needed) {
    if (!size) g.Store32(size_ptr, needed);
    return kErrorInsufficientBuffer;
  }
  if (!xuid_count && user != 0) return Finish(g, overlapped, kErrorNoSuchUser);
  std::memset(g.host(buffer), 0, needed);
  // X_USER_READ_PROFILE_SETTINGS: the count and a pointer to the settings that follow.
  g.Store32(buffer, setting_count * copies);
  g.Store32(buffer + 4, buffer + 8);
  uint32_t data = buffer + 8 + kXuserSize * setting_count * copies;
  for (uint32_t c = 0; c < copies; ++c) {
    for (uint32_t i = 0; i < setting_count; ++i) {
      const uint32_t at = buffer + 8 + kXuserSize * (c * setting_count + i), id = g.Load32(setting_ids + 4 * i);
      const auto stored = std::find_if(settings_.begin(), settings_.end(), [&](const Setting& s) { return s.id == id; });
      g.Store32(at + 0, stored != settings_.end() ? 2 : 1);  // from: title-written or default
      if (xuid_count) g.Store64(at + 8, title_.xuid);
      else g.Store32(at + 8, user);
      g.Store32(at + 16, id);
      g.Store8(at + 24, uint8_t(id >> 28));  // data type
      const uint32_t type = id >> 28, bytes = (id >> 16) & 0xFFF;
      if (stored != settings_.end()) std::memcpy(g.host(at + 24), stored->data.data(), 16);
      if (type == 4 || type == 6) {
        g.Store8(at + 24, uint8_t(type));
        g.Store32(at + 32, bytes);
        g.Store32(at + 36, data);
        if (stored != settings_.end() && stored->data.size() > 16) {
          std::memcpy(g.host(data), stored->data.data() + 16, std::min<size_t>(bytes, stored->data.size() - 16));
        }
        data += bytes;
      }
    }
  }
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::UserWriteProfileSettings(Guest g, uint32_t, uint32_t user, uint32_t setting_count, uint32_t settings,
                                       uint32_t overlapped) {
  if (!setting_count || !settings) return kErrorInvalidParameter;
  if (user != 0) return Finish(g, overlapped, kErrorNoSuchUser);
  for (uint32_t i = 0; i < setting_count; ++i) {
    const uint32_t at = settings + kXuserSize * i, id = g.Load32(at + 16), type = g.Load8(at + 24);
    if (type == 0) continue;
    Setting s{id, std::vector<uint8_t>(g.host(at + 24), g.host(at + 24) + 16)};
    if (type == 4 || type == 6) {
      const uint32_t bytes = g.Load32(at + 32), pointer = g.Load32(at + 36);
      if (pointer) s.data.insert(s.data.end(), g.host(pointer), g.host(pointer) + bytes);
    }
    auto it = std::find_if(settings_.begin(), settings_.end(), [&](const Setting& x) { return x.id == id; });
    if (it != settings_.end()) *it = std::move(s);
    else settings_.push_back(std::move(s));
  }
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::SessionCreateHandle(Guest g, uint32_t handle_ptr) {
  const uint32_t object = Objects().AllocateObject(16);
  if (!object) return kErrorInvalidParameter;
  std::memset(g.host(object), 0, 16);
  g.Store32(handle_ptr, Objects().OpenHandle(object));
  return kErrorSuccess;
}

uint32_t Xam::SessionRefObjByHandle(Guest g, uint32_t handle, uint32_t object_ptr) {
  const uint32_t object = Objects().Lookup(handle);
  if (!object) return 0xC0000008;  // STATUS_INVALID_HANDLE
  g.Store32(object_ptr, object);
  return kStatusSuccess;
}

uint32_t Xam::ShowSigninUI(uint32_t, uint32_t) {
  // The local profile is already signed in: the guide opens and closes at once.
  Notify(kNotifySystemUi, 1);
  Notify(kNotifySystemUi, 0);
  return kErrorSuccess;
}

uint32_t Xam::ShowLiveUI() {
  Notify(kNotifySystemUi, 1);
  Notify(kNotifySystemUi, 0);
  return kErrorSuccess;
}

uint32_t Xam::UserAreUsersFriends(Guest g, uint32_t user, uint32_t out, uint32_t overlapped) {
  const uint32_t result = user >= 4 ? kErrorInvalidParameter : user != 0 ? kErrorNoSuchUser : kErrorSuccess;
  if (out) {
    g.Store32(out, 0);
    return result;
  }
  return Finish(g, overlapped, result == kErrorSuccess ? kErrorSuccess : kErrorFunctionFailed);
}

uint32_t Xam::UserCreateAchievementEnumerator(Guest g, uint32_t user, uint32_t flags, uint32_t count, uint32_t size_ptr,
                                              uint32_t handle_ptr) {
  if (!count || !size_ptr || !handle_ptr || user >= 4) return kErrorInvalidParameter;
  // XACHIEVEMENT_DETAILS (0x24 bytes), with room for its three strings when details are asked for.
  const uint32_t item_size = 0x24 + ((flags & 7) ? 0x8A8 : 0);
  g.Store32(size_ptr, item_size * count);
  g.Store32(handle_ptr, CreateEnumerator(g, item_size, count, {}));
  return kErrorSuccess;
}

uint32_t Xam::ParseGamerTileKey(Guest g, uint32_t, uint32_t out1, uint32_t out2, uint32_t out3) {
  g.Store32(out1, 0xC0DE0001);
  g.Store32(out2, 0xC0DE0002);
  g.Store32(out3, 0xC0DE0003);
  return kErrorSuccess;
}

uint32_t Xam::ReadTileToTexture(Guest g, uint64_t tile_id, uint32_t buffer, uint32_t stride, uint32_t height, uint32_t overlapped) {
  if (!tile_id) return kErrorInvalidParameter;
  std::memset(g.host(buffer), 0xFF, size_t(stride) * height);  // opaque white
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::TitleId(Guest g) const {
  const uint32_t info = title_.xex_header ? ImageXexHeaderField(g, title_.xex_header, 0x00040006) : 0;
  return info ? g.Load32(info + 12) : 0;  // XEX_HEADER_EXECUTION_INFO: media id, version, base version, title id
}

uint32_t Xam::TaskSchedule(Guest g, uint32_t callback, uint32_t message, uint32_t handle_ptr) {
  return Threads().CreateThread(g, handle_ptr, 0, 0, 0, callback, message, 0);
}

uint32_t Xam::VoiceCreate(Guest g, uint32_t voice_ptr) {
  if (voice_ptr) g.Store32(voice_ptr, 0);
  return kErrorAccessDenied;
}

uint32_t Xam::ShowMessageBoxUI(Guest g, uint32_t result_ptr, uint32_t overlapped) {
  if (result_ptr) g.Store32(result_ptr, 0);  // the first button
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::ShowKeyboardUI(Guest g, uint32_t default_text, uint32_t buffer, uint32_t length, uint32_t overlapped) {
  if (!buffer || !length) return kErrorInvalidParameter;
  Notify(kNotifySystemUi, 1);
  uint32_t i = 0;
  if (default_text) {
    for (; i + 1 < length; ++i) {
      const uint16_t c = g.Load16(default_text + 2 * i);
      if (!c) break;
      g.Store16(buffer + 2 * i, c);
    }
  }
  std::memset(g.host(buffer + 2 * i), 0, size_t(length - i) * 2);
  Notify(kNotifySystemUi, 0);
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::ShowDeviceSelectorUI(Guest g, uint32_t device_id_ptr, uint32_t overlapped) {
  if (device_id_ptr) g.Store32(device_id_ptr, 1);  // the hard drive
  return Finish(g, overlapped, kErrorSuccess);
}

// --- enumerators --------------------------------------------------------------------------------------------------

uint32_t Xam::CreateEnumerator(Guest g, uint32_t item_size, uint32_t per_call, std::vector<std::vector<uint8_t>> items) {
  const uint32_t object = Objects().AllocateObject(16);
  if (!object) return 0;
  std::memset(g.host(object), 0, 16);
  enumerators_.push_back({object, item_size, std::max<uint32_t>(1, per_call), 0, std::move(items)});
  return Objects().OpenHandle(object);
}

Xam::Enumerator* Xam::FindEnumerator(uint32_t handle) {
  const uint32_t object = Objects().Lookup(handle);
  for (Enumerator& e : enumerators_) {
    if (object && e.object == object) return &e;
  }
  return nullptr;
}

uint32_t Xam::Enumerate(Guest g, uint32_t handle, uint32_t, uint32_t buffer, uint32_t length, uint32_t items_ptr, uint32_t overlapped) {
  Enumerator* e = FindEnumerator(handle);
  if (!e) return kErrorInvalidParameter;
  if (items_ptr) g.Store32(items_ptr, 0);
  if (length < e->item_size) return Finish(g, overlapped, kErrorInsufficientBuffer);
  uint32_t n = 0;
  while (e->next < e->items.size() && n < e->per_call && (n + 1) * e->item_size <= length) {
    std::memcpy(g.host(buffer + n * e->item_size), e->items[e->next].data(), std::min<size_t>(e->item_size, e->items[e->next].size()));
    ++e->next;
    ++n;
  }
  if (items_ptr) g.Store32(items_ptr, n);
  return Finish(g, overlapped, n ? kErrorSuccess : kErrorNoMoreFiles, n);
}

uint32_t Xam::GetPrivateEnumStructure(Guest g, uint32_t handle, uint32_t out) {
  Enumerator* e = FindEnumerator(handle);
  if (!e) return 0xC0000008;
  if (out) g.Store32(out, e->object);
  return kStatusSuccess;
}

// --- content ------------------------------------------------------------------------------------------------------

std::filesystem::path Xam::ContentPath(Guest g, uint32_t content_data) const {
  char type[16];
  std::snprintf(type, sizeof type, "%08X", g.Load32(content_data + 4));
  std::string name = GuestString(g, content_data + kContentFileNameOffset, 42);
  for (char& c : name) {
    if (c == '/' || c == '\\' || c == ':') c = '_';
  }
  return title_.content_root / type / name;
}

uint32_t Xam::ContentCreate(Guest g, uint32_t user, uint32_t root_name, uint32_t content_data, uint32_t flags, uint32_t disposition_ptr,
                            uint32_t license_ptr, uint32_t overlapped) {
  (void)user;
  if (!content_data || !root_name || title_.content_root.empty()) return Finish(g, overlapped, kErrorInvalidParameter);
  const std::filesystem::path path = ContentPath(g, content_data);
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  // XCONTENTFLAG creation modes: create new, create always, open existing, open always, truncate existing.
  const uint32_t mode = flags & 0xF;
  uint32_t result = kErrorSuccess, disposition = exists ? 2 : 1;  // opened / created
  if ((mode == 1 && exists)) result = kErrorAlreadyExists;
  else if ((mode == 3 || mode == 5) && !exists) result = kErrorFileNotFound;
  else {
    if (exists && (mode == 2 || mode == 5)) std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
    const std::string root = GuestString(g, root_name, 64);
    Files().Mount("\\Device\\Content\\" + root, path, false);
    Files().Link(root + ":", "\\Device\\Content\\" + root);
    open_content_.push_back(root);
  }
  if (disposition_ptr) g.Store32(disposition_ptr, result ? 0 : disposition);
  if (license_ptr) g.Store32(license_ptr, 0);
  return Finish(g, overlapped, result);
}

uint32_t Xam::ContentClose(Guest g, uint32_t root_name, uint32_t overlapped) {
  const std::string root = GuestString(g, root_name, 64);
  auto it = std::find(open_content_.begin(), open_content_.end(), root);
  if (it == open_content_.end()) return Finish(g, overlapped, kErrorFileNotFound);
  open_content_.erase(it);
  Files().Unlink(root + ":");
  Files().Unmount("\\Device\\Content\\" + root);
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::ContentDelete(Guest g, uint32_t, uint32_t content_data, uint32_t overlapped) {
  std::error_code error;
  const std::filesystem::path path = ContentPath(g, content_data);
  if (!std::filesystem::exists(path, error)) return Finish(g, overlapped, kErrorFileNotFound);
  std::filesystem::remove_all(path, error);
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::ContentCreateEnumerator(Guest g, uint32_t, uint32_t device_id, uint32_t content_type, uint32_t, uint32_t per_call,
                                      uint32_t size_ptr, uint32_t handle_out) {
  if (!handle_out) return kErrorInvalidParameter;
  std::vector<std::vector<uint8_t>> items;
  char type[16];
  std::snprintf(type, sizeof type, "%08X", content_type);
  std::error_code error;
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(title_.content_root / type, error)) {
    if (entry.is_directory()) names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  for (const std::string& name : names) {
    std::vector<uint8_t> item(kContentDataSize, 0);
    const uint32_t device = device_id ? device_id : 1;
    const uint8_t be_device[4] = {uint8_t(device >> 24), uint8_t(device >> 16), uint8_t(device >> 8), uint8_t(device)};
    const uint8_t be_type[4] = {uint8_t(content_type >> 24), uint8_t(content_type >> 16), uint8_t(content_type >> 8), uint8_t(content_type)};
    std::memcpy(item.data(), be_device, 4);
    std::memcpy(item.data() + 4, be_type, 4);
    for (size_t i = 0; i < name.size() && i < 127; ++i) item[8 + 2 * i + 1] = uint8_t(name[i]);  // display name (UTF-16BE)
    std::memcpy(item.data() + kContentFileNameOffset, name.data(), std::min<size_t>(name.size(), 41));
    items.push_back(std::move(item));
  }
  if (size_ptr) g.Store32(size_ptr, kContentDataSize * std::max<uint32_t>(1, per_call));
  const uint32_t handle = CreateEnumerator(g, kContentDataSize, per_call, std::move(items));
  if (!handle) return kErrorInvalidParameter;
  g.Store32(handle_out, handle);
  return kErrorSuccess;
}

uint32_t Xam::ContentSetThumbnail(Guest g, uint32_t content_data, uint32_t buffer, uint32_t size, uint32_t overlapped) {
  if (!content_data || title_.content_root.empty()) return Finish(g, overlapped, kErrorInvalidParameter);
  const std::filesystem::path path = ContentPath(g, content_data);
  std::error_code error;
  if (!std::filesystem::is_directory(path, error)) return Finish(g, overlapped, kErrorFileNotFound);
  std::ofstream stream(path.parent_path() / (path.filename().string() + ".png"), std::ios::binary | std::ios::trunc);
  if (size) stream.write(reinterpret_cast<const char*>(g.host(buffer)), size);
  return Finish(g, overlapped, stream ? kErrorSuccess : kErrorAccessDenied);
}

uint32_t Xam::ContentGetLicenseMask(Guest g, uint32_t mask_ptr, uint32_t overlapped) {
  if (!mask_ptr) return Finish(g, overlapped, kErrorInvalidParameter);
  g.Store32(mask_ptr, title_.license_mask);
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::ContentGetDeviceState(Guest g, uint32_t device_id, uint32_t overlapped) {
  return Finish(g, overlapped, device_id <= 1 ? kErrorSuccess : kErrorDeviceNotConnected);
}

// --- controllers --------------------------------------------------------------------------------------------------

namespace {

uint32_t ActualUser(uint32_t user, uint32_t flags) { return ((user & 0xFF) == 0xFF || (flags & 0x40000000)) ? 0 : user; }

}  // namespace

uint32_t Xam::InputGetCapabilities(Guest g, uint32_t user, uint32_t flags, uint32_t caps) {
  if (!caps) return kErrorBadArguments;
  if ((flags & 0xFF) && !(flags & kInputFlagGamepad)) return kErrorDeviceNotConnected;
  const uint32_t u = ActualUser(user, flags);
  const bool connected = host_.controller_connected ? host_.controller_connected(u) : u == 0;
  if (!connected) return kErrorDeviceNotConnected;
  // XINPUT_CAPABILITIES: a gamepad with every button, both triggers and sticks at full range, both motors.
  std::memset(g.host(caps), 0, 20);
  g.Store8(caps + 0, 1);
  g.Store8(caps + 1, 1);
  g.Store16(caps + 4, 0xF3FF);
  g.Store8(caps + 6, 0xFF);
  g.Store8(caps + 7, 0xFF);
  for (uint32_t i = 0; i < 4; ++i) g.Store16(caps + 8 + 2 * i, 0xFFC0);
  g.Store16(caps + 16, 0xFFFF);
  g.Store16(caps + 18, 0xFFFF);
  return kErrorSuccess;
}

uint32_t Xam::InputGetState(Guest g, uint32_t user, uint32_t flags, uint32_t state) {
  if ((flags & 0xFF) && !(flags & kInputFlagGamepad)) return kErrorDeviceNotConnected;
  const uint32_t u = ActualUser(user, flags);
  const bool connected = host_.controller_connected ? host_.controller_connected(u) : u == 0;
  if (!connected) return kErrorDeviceNotConnected;
  if (!state) return kErrorSuccess;  // a query
  GamepadState pad;
  if (host_.controller_state) host_.controller_state(u, pad);
  g.Store32(state, ++packet_number_);
  g.Store16(state + 4, pad.buttons);
  g.Store8(state + 6, pad.left_trigger);
  g.Store8(state + 7, pad.right_trigger);
  g.Store16(state + 8, uint16_t(pad.thumb_lx));
  g.Store16(state + 10, uint16_t(pad.thumb_ly));
  g.Store16(state + 12, uint16_t(pad.thumb_rx));
  g.Store16(state + 14, uint16_t(pad.thumb_ry));
  return kErrorSuccess;
}

uint32_t Xam::InputSetState(Guest g, uint32_t user, uint32_t vibration) {
  if (!vibration) return kErrorBadArguments;
  const uint32_t u = (user & 0xFF) == 0xFF ? 0 : user;
  const bool connected = host_.controller_connected ? host_.controller_connected(u) : u == 0;
  if (!connected) return kErrorDeviceNotConnected;
  if (host_.vibrate) host_.vibrate(u, g.Load16(vibration), g.Load16(vibration + 2));
  return kErrorSuccess;
}

uint32_t Xam::InputGetKeystroke(Guest g, uint32_t user, uint32_t flags, uint32_t keystroke) {
  if (!keystroke) return kErrorBadArguments;
  if ((flags & 0xFF) && !(flags & kInputFlagGamepad)) return kErrorDeviceNotConnected;
  const uint32_t u = ActualUser(user, flags);
  const bool connected = host_.controller_connected ? host_.controller_connected(u) : u == 0;
  if (!connected) return kErrorDeviceNotConnected;
  std::memset(g.host(keystroke), 0, 8);
  return kErrorEmpty;  // keystrokes come from button transitions, which the input source does not report yet
}

// --- the title module ---------------------------------------------------------------------------------------------

uint32_t Xam::ImageXexHeaderField(Guest g, uint32_t header, uint32_t key) const {
  // XEX2: the optional headers follow at +0x18 as (key, value) pairs, their count at +0x14. The low byte of the key is
  // the size in words: 0 = the value itself, 1 = the value's address, otherwise an offset from the header.
  if (!header) return 0;
  const uint32_t count = g.Load32(header + 0x14);
  for (uint32_t i = 0; i < count && i < 0x400; ++i) {
    const uint32_t at = header + 0x18 + 8 * i;
    if (g.Load32(at) != key) continue;
    switch (key & 0xFF) {
      case 0x00: return g.Load32(at + 4);
      case 0x01: return at + 4;
      default: return header + g.Load32(at + 4);
    }
  }
  return 0;
}

uint32_t Xam::GetModuleHandle(Guest g, uint32_t name, uint32_t handle_ptr) {
  const std::string module = GuestString(g, name, 64);
  std::string lower = module;
  for (char& c : lower) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
  uint32_t handle = 0;
  if (module.empty() || lower == "default.xex" || lower == title_.module_name) handle = title_.module_handle;
  else if (lower == "xam.xex" || lower == "xboxkrnl.exe") handle = SystemModule(g);
  if (handle_ptr) g.Store32(handle_ptr, handle);
  return handle ? kStatusSuccess : kStatusNotFound;
}

uint32_t Xam::SystemModule(Guest g) {
  if (!system_module_) {
    system_module_ = Objects().AllocateObject(0x60);  // LDR_DATA_TABLE_ENTRY-sized
    if (system_module_) std::memset(g.host(system_module_), 0, 0x60);
  }
  return system_module_;
}

uint32_t Xam::LoadImage(Guest g, uint32_t name, uint32_t handle_ptr) {
  std::string path = GuestString(g, name, 260);
  for (char& c : path) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
  const size_t slash = path.find_last_of("\\/:");
  const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
  uint32_t handle = 0;
  if (file == "default.xex" || file == title_.module_name) handle = title_.module_handle;
  else if (file == "xam.xex" || file == "xboxkrnl.exe") handle = SystemModule(g);
  if (handle_ptr) g.Store32(handle_ptr, handle);
  return handle ? kStatusSuccess : kStatusNoSuchFile;
}

uint32_t Xam::UnloadImage(uint32_t handle) {
  if (handle && (handle == title_.module_handle || handle == system_module_)) return kStatusSuccess;
  return kStatusInvalidHandle;
}

uint32_t Xam::LoadImageHeaders(Guest g, uint32_t path, uint32_t buffer, uint32_t size) {
  constexpr uint32_t kStatusInvalidImageFormat = 0xC000007B, kStatusBufferTooSmall = 0xC0000023, kXex2 = 0x58455832;
  const uint16_t length = path ? g.Load16(path) : 0;
  const uint32_t pointer = path ? g.Load32(path + 4) : 0;
  if (!pointer || !length) return kStatusInvalidParameter;
  const std::filesystem::path host = Files().Resolve(std::string(reinterpret_cast<const char*>(g.host(pointer)), length));
  std::ifstream stream(host, std::ios::binary);
  if (host.empty() || !stream) return kStatusNoSuchFile;
  uint8_t head[0x18] = {};
  stream.read(reinterpret_cast<char*>(head), sizeof head);
  const auto be32 = [&](size_t at) { return uint32_t(head[at]) << 24 | uint32_t(head[at + 1]) << 16 | uint32_t(head[at + 2]) << 8 | head[at + 3]; };
  if (stream.gcount() != std::streamsize(sizeof head) || be32(0) != kXex2) return kStatusInvalidImageFormat;
  const uint32_t header_size = be32(8);  // the headers end where the PE image begins
  if (header_size < sizeof head) return kStatusInvalidImageFormat;
  if (header_size > size) return kStatusBufferTooSmall;
  stream.seekg(0);
  stream.read(reinterpret_cast<char*>(g.host(buffer)), header_size);
  return stream.gcount() == std::streamsize(header_size) ? kStatusSuccess : kStatusInvalidImageFormat;
}

uint32_t Xam::GetProcedureAddress(Guest g, uint32_t module, uint32_t ordinal, uint32_t out) {
  if (out) g.Store32(out, 0);
  uint32_t address = 0;
  if (host_.procedure && module != title_.module_handle) address = host_.procedure(module ? "system" : "", ordinal);
  if (!address) return kStatusProcedureNotFound;
  if (out) g.Store32(out, address);
  return kStatusSuccess;
}

uint32_t Xam::CheckExecutablePrivilege(Guest g, uint32_t privilege) const {
  // XEX_HEADER_SYSTEM_FLAGS (0x00030000) holds privileges 0-31, XEX_HEADER_SYSTEM_FLAGS_32 (0x00030100) 32-63.
  if (privilege >= 64) return 0;
  const uint32_t flags = ImageXexHeaderField(g, title_.xex_header, privilege < 32 ? 0x00030000 : 0x00030100);
  return (flags >> (privilege & 31)) & 1;
}

uint32_t Xam::GetExecutionId(Guest g, uint32_t info_ptr) const {
  const uint32_t field = ImageXexHeaderField(g, title_.xex_header, 0x00040006);  // XEX_HEADER_EXECUTION_INFO
  if (!field) return kStatusNotFound;
  g.Store32(info_ptr, field);
  return kStatusSuccess;
}

void Xam::RegisterTitleTerminateNotification(uint32_t registration, uint32_t create) {
  auto it = std::find(terminate_notifications_.begin(), terminate_notifications_.end(), registration);
  if (create && it == terminate_notifications_.end()) terminate_notifications_.push_back(registration);
  if (!create && it != terminate_notifications_.end()) terminate_notifications_.erase(it);
}

// --- launch data, sections, devices, pool ---------------------------------------------------------------------------

uint32_t Xam::LoaderSetLaunchData(Guest g, uint32_t data, uint32_t size) {
  launch_data_present_ = size != 0;
  launch_data_.assign(g.host(data), g.host(data) + size);
  return kErrorSuccess;
}

uint32_t Xam::LoaderGetLaunchDataSize(Guest g, uint32_t size_ptr) {
  if (!size_ptr) return kErrorInvalidParameter;
  g.Store32(size_ptr, launch_data_present_ ? uint32_t(launch_data_.size()) : 0);
  return launch_data_present_ ? kErrorSuccess : kErrorNotFound;
}

uint32_t Xam::LoaderGetLaunchData(Guest g, uint32_t buffer, uint32_t size) {
  if (!launch_data_present_) return kErrorNotFound;
  std::memcpy(g.host(buffer), launch_data_.data(), std::min<size_t>(size, launch_data_.size()));
  return kErrorSuccess;
}

uint32_t Xam::GetModuleSection(Guest g, uint32_t module, uint32_t name, uint32_t data_ptr, uint32_t size_ptr) {
  if (module != title_.module_handle || !title_.xex_header) return 0xC0000008;  // STATUS_INVALID_HANDLE
  // XEX_HEADER_RESOURCE_INFO: the total size, then (8-character name, address, size) per section.
  const uint32_t info = ImageXexHeaderField(g, title_.xex_header, 0x000002FF);
  if (!info) return kStatusNotFound;
  const std::string wanted = GuestString(g, name, 8);
  const uint32_t count = (g.Load32(info) - 4) / 16;
  for (uint32_t i = 0; i < count && i < 256; ++i) {
    const uint32_t entry = info + 4 + 16 * i;
    if (GuestString(g, entry, 8) != wanted) continue;
    if (data_ptr) g.Store32(data_ptr, g.Load32(entry + 8));
    if (size_ptr) g.Store32(size_ptr, g.Load32(entry + 12));
    return kStatusSuccess;
  }
  return kStatusNotFound;
}

uint32_t Xam::ContentGetDeviceData(Guest g, uint32_t device_id, uint32_t data) {
  if (device_id != 1 && device_id != 2) return kErrorDeviceNotConnected;
  // XDEVICE_DATA: id, type, total bytes, free bytes, 28 wide characters of name.
  std::memset(g.host(data), 0, 80);
  uint64_t total = 20ull << 30, available = 3ull << 30;
  std::error_code error;
  if (device_id == 1 && !title_.content_root.empty()) {
    const auto space = std::filesystem::space(title_.content_root.root_path(), error);
    if (!error) {
      total = space.capacity;
      available = space.available;
    }
  }
  if (device_id == 2) {
    total = 7ull << 30;
    available = 0;
  }
  g.Store32(data, device_id);
  g.Store32(data + 4, device_id);  // 1 hard drive, 2 optical disc
  g.Store64(data + 8, total);
  g.Store64(data + 16, available);
  const char* label = device_id == 1 ? "Hard Drive" : "Disc";
  for (uint32_t i = 0; label[i]; ++i) g.Store16(data + 24 + 2 * i, uint16_t(label[i]));
  return kErrorSuccess;
}

uint32_t Xam::ContentGetCreator(Guest g, uint32_t, uint32_t content_data, uint32_t is_creator_ptr, uint32_t xuid_ptr, uint32_t overlapped) {
  std::error_code error;
  if (!std::filesystem::exists(ContentPath(g, content_data), error)) return Finish(g, overlapped, kErrorFileNotFound);
  // The local profile made its saved games; other content (DLC) has no creator.
  const bool saved_game = g.Load32(content_data + 4) == 1;
  if (is_creator_ptr) g.Store32(is_creator_ptr, saved_game ? 1 : 0);
  if (xuid_ptr) g.Store64(xuid_ptr, saved_game ? title_.xuid : 0);
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::SetCacheElementCount(uint32_t count) {
  cache_elements_ = count;
  return kStatusSuccess;
}

uint32_t Xam::AllocatePool(Guest g, uint32_t size) { return Memory().AllocatePages(g, size); }

void Xam::FreePool(uint32_t address) { Memory().FreePages(address & ~0xFFFu); }

uint32_t Xam::CreateDevice(Guest g, uint32_t device_out) {
  const uint32_t device = Objects().AllocateObject(0x24), extension = AllocatePool(g, 0x1000);
  if (!device || !extension) return 0xC0000017;
  std::memset(g.host(device), 0, 0x24);
  g.Store32(device + 0x18, extension);
  if (device_out) g.Store32(device_out, device);
  return kStatusSuccess;
}

// --- stops --------------------------------------------------------------------------------------------------------

void Xam::Stop(const char* reason, uint32_t code) { (host_.stop ? host_.stop : DefaultStop)(reason, code); }

uint32_t Xam::RaiseException(Guest g, uint32_t record) {
  const uint32_t code = record ? g.Load32(record) : 0;
  if (code == 0x406D1388) return 0;  // SetThreadName: a debugger convention, nothing to do
  Stop("unhandled guest exception", code);
  return 0;
}

// --- network ------------------------------------------------------------------------------------------------------

uint32_t Xam::NetStartup() { return 0; }

uint32_t Xam::WsaStartup(Guest g, uint32_t version, uint32_t data) {
  if (data) {
    // WSADATA: version, high version, then descriptions the titles do not read.
    std::memset(g.host(data), 0, 0x190);
    g.Store16(data, uint16_t(version));
    g.Store16(data + 2, 0x0202);
  }
  return 0;
}

uint32_t Xam::Socket() {
  t_wsa_error = kWsaENetDown;  // no network
  return 0xFFFFFFFF;
}

uint32_t Xam::SocketFailure(uint32_t) {
  t_wsa_error = kWsaENotSock;
  return 0xFFFFFFFF;
}

uint32_t Xam::WsaGetLastError() const { return t_wsa_error; }

uint32_t Xam::WsaCreateEvent() {
  const uint32_t event = Objects().AllocateObject(kEventSize);
  if (!event) return 0;
  Objects().InitializeEvent(Objects().guest(), event, kNotificationEvent, 0);
  return Objects().OpenHandle(event);
}

uint32_t Xam::WsaCloseEvent(uint32_t handle) {
  if (Objects().Close(handle) == kStatusSuccess) return 1;
  t_wsa_error = kWsaInvalidHandle;
  return 0;
}

uint32_t Xam::WsaWaitForMultipleEvents(Guest g, uint32_t count, uint32_t events, uint32_t wait_all, uint32_t timeout_ms,
                                       uint32_t thread) {
  if (!count || count > 64) {
    t_wsa_error = kWsaInvalidParameter;
    return kWsaWaitFailed;
  }
  // Milliseconds as a relative NT timeout (negative, 100 ns units); none for ever.
  const int64_t relative = -int64_t(timeout_ms) * 10000;
  const uint32_t status =
      Objects().WaitForHandles(g, count, events, wait_all ? 0 : 1, thread, timeout_ms == 0xFFFFFFFF ? nullptr : &relative);
  if (status == kStatusTimeout) return kWsaWaitTimeout;
  if (status < 64) return status;  // WAIT_0 + index (wait-any), 0 (wait-all)
  t_wsa_error = kWsaInvalidHandle;
  return kWsaWaitFailed;
}

uint32_t Xam::XNetRandom(Guest g, uint32_t buffer, uint32_t length) {
  thread_local std::mt19937 generator{std::random_device{}()};
  uint8_t* out = g.host(buffer);
  for (uint32_t i = 0; i < length; ++i) out[i] = uint8_t(generator());
  return 0;
}

uint32_t Xam::XNetCreateKey(Guest g, uint32_t key_id, uint32_t key) {
  // XNKID (8 bytes) and XNKEY (16 bytes), random; the key id's high nibble says what kind it is: 0, system link (the
  // only kind a console without Live creates).
  XNetRandom(g, key_id, 8);
  XNetRandom(g, key, 16);
  g.Store8(key_id, g.Load8(key_id) & 0x0F);
  return 0;
}

uint32_t Xam::XNetQosRelease(uint32_t qos) {
  if (!qos) return kStatusInvalidParameter;
  FreePool(qos);
  return 0;
}

uint32_t Xam::XNetGetTitleXnAddr(Guest g, uint32_t address) {
  if (address) std::memset(g.host(address), 0, 36);
  return 1;  // XNET_GET_XNADDR_NONE: no address
}

}  // namespace any360::sys
