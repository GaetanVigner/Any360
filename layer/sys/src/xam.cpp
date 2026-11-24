#include "any360/sys/xam.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>

#include "any360/sys/config.h"
#include "any360/sys/files.h"
#include "any360/sys/info.h"
#include "any360/sys/memory.h"
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"

namespace any360::sys {

// host_times.cpp: a path's times, size and kind in one question to the host.
bool HostFileStat(const std::filesystem::path& path, uint64_t& creation, uint64_t& access, uint64_t& write, uint64_t& size,
                  bool& directory);

uint32_t RtlNtStatusToDosError(uint32_t status);  // config.h

namespace {

constexpr uint32_t kStatusNotFound = 0xC0000225, kStatusProcedureNotFound = 0xC000007A;
constexpr uint32_t kXuserSize = 40;                    // X_USER_PROFILE_SETTING
constexpr uint32_t kContentDataSize = 0x134, kContentAggregateSize = 0x148;           // XCONTENT_DATA: device, type, 128 wide chars, 42-byte file name
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
  online_schema_ = 0;
  std::memset(dash_app_info_, 0, sizeof dash_app_info_);
  dash_context_ = 0;
  nui_session_id_ = 0;
  user_preferences_ = 0;
  profile_mounts_.clear();
  profile_dir_.clear(), profile_dir_root_.clear();
  language_fallback_ = 0;
  iptv_name_ = 0;
  std::memset(typeface_patch_at_, 0, sizeof typeface_patch_at_);
  smc_tray_ = 2;
  smc_cpu_fan_ = smc_gpu_fan_ = 100;
  smc_power_led_ = 0x0100, smc_leds_ = 0x8001;
  content_change_callback_ = launch_request_callback_ = nui_tilt_callback_ = 0;
  close_hook_ = Objects().AddCloseHook([](uint32_t object) { return XamState().OnClose(object); });
  system_module_ = 0;
  settings_.clear();
  terminate_notifications_.clear();
  open_content_.clear();
  StopInputSampling();
  input_step_ = InputQueue::kConsoleFrame;
  {
    std::lock_guard lock(pads_mutex_);
    pads_ = {};
  }
  launch_data_.clear();
  launch_data_present_ = false;
  cache_elements_ = 0;
  startup_params_.clear();
}

// --- overlapped ------------------------------------------------------------------------------------------------

void Xam::Complete(Guest g, uint32_t overlapped, uint32_t result, uint32_t length, bool hresult) {
  // XOVERLAPPED: result, length, context, event, completion routine and context, extended error.
  g.Store32(overlapped + 0x00, result);
  g.Store32(overlapped + 0x04, length);
  g.Store32(overlapped + 0x18, hresult && result ? 0x80070000u | (result & 0xFFFF) : result);
  const uint32_t event = g.Load32(overlapped + 0x0C);
  if (event) Objects().NtSetEvent(g, event, 0);
}

uint32_t Xam::Finish(Guest g, uint32_t overlapped, uint32_t result, uint32_t length, bool hresult) {
  if (!overlapped) return result;
  Complete(g, overlapped, result, length, hresult);
  return kErrorIoPending;
}

// --- messages and notifications ------------------------------------------------------------------------------------

uint32_t Xam::MsgStartIORequest(Guest g, uint32_t, uint32_t, uint32_t overlapped, uint32_t, uint32_t) {
  // The system applications (presence, achievements, marketplace) accept every request of an offline title.
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::MsgInProcessCall(uint32_t, uint32_t, uint32_t, uint32_t) { return kErrorSuccess; }

uint32_t Xam::MsgCancelIORequest(Guest, uint32_t, uint32_t) { return kErrorSuccess; }

uint32_t Xam::MsgSystemProcessCall(uint32_t app, uint32_t message, uint32_t buffer, uint32_t length) {
  return MsgInProcessCall(app, message, buffer, length);
}

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

bool Xam::OnClose(uint32_t object) {
  // An object is in each list once at most, and the one closed is most often the last made: searched from the end.
  auto listener = std::find_if(listeners_.rbegin(), listeners_.rend(), [&](const Listener& l) { return l.object == object; });
  if (listener != listeners_.rend()) {
    listeners_.erase(std::next(listener).base());
    return true;
  }
  auto enumerator = std::find_if(enumerators_.rbegin(), enumerators_.rend(), [&](const Enumerator& e) { return e.object == object; });
  if (enumerator == enumerators_.rend()) return false;
  enumerators_.erase(std::next(enumerator).base());
  return true;
}

uint32_t Xam::UserGetGamerTag(Guest g, uint32_t user, uint32_t buffer, uint32_t length) {
  if (user >= 4) return 0x80070057;  // E_INVALIDARG
  if (user) return 0x80070000 | kErrorNoSuchUser;
  if (!buffer || length < 16) return 0x80070057;
  const std::string& tag = title_.gamertag;
  const uint32_t n = std::min<uint32_t>(uint32_t(tag.size()), std::min<uint32_t>(length, 16) - 1);
  for (uint32_t i = 0; i < n; ++i) g.Store16(buffer + 2 * i, uint8_t(tag[i]));
  g.Store16(buffer + 2 * n, 0);
  return 0;
}

uint32_t Xam::UserContentRestrictionGetFlags(Guest g, uint32_t user, uint32_t flags_out) {
  if (user) return kErrorNoSuchUser;
  g.Store32(flags_out, 0);
  return kErrorSuccess;
}

uint32_t Xam::UserContentRestrictionGetRating(Guest g, uint32_t user, uint32_t rating_out, uint32_t extra_out) {
  if (user) return kErrorNoSuchUser;
  g.Store32(rating_out, 0x3F);  // no rating limit, as the reference answers
  g.Store32(extra_out, 0);
  return kErrorSuccess;
}

uint32_t Xam::UserContentRestrictionCheckAccess(Guest g, uint32_t allowed_out, uint32_t overlapped) {
  g.Store32(allowed_out, 1);
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::GetOnlineSchema(Guest g) {
  // An empty schema: counts of zero and section offsets all at its end (0x2C), as the reference has it.
  static constexpr uint8_t kSchema[] = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00,
      0x00, 0x2C, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18,
  };
  if (!online_schema_) {
    const uint32_t at = Objects().AllocateObject(8 + sizeof kSchema);
    if (!at) return 0;
    std::memcpy(g.host(at + 8), kSchema, sizeof kSchema);
    g.Store32(at, at + 8);
    g.Store32(at + 4, sizeof kSchema);
    online_schema_ = at;
  }
  return online_schema_;
}

void Xam::GetActiveDashAppInfo(Guest g, uint32_t info) const {
  if (info) std::memcpy(g.host(info), dash_app_info_, sizeof dash_app_info_);
}

void Xam::SetActiveDashAppInfo(Guest g, uint32_t info) {
  if (info) {
    std::memcpy(dash_app_info_, g.host(info), sizeof dash_app_info_);
  } else {
    std::memset(dash_app_info_, 0, sizeof dash_app_info_);
  }
}

uint32_t Xam::SetDashContext(uint32_t context) {
  constexpr uint32_t kNotifyDashContextChanged = 0x8000000C;
  if (context != dash_context_) {
    dash_context_ = context;
    Notify(kNotifyDashContextChanged, 0);
  }
  return 0;
}

uint32_t Xam::ShowCreateProfileUI() { return ShowLiveUI(); }

uint32_t Xam::ShowProfileUI(uint32_t user) { return UserGetSigninState(user) ? ShowLiveUI() : kErrorAccessDenied; }

uint32_t Xam::ShowForcedNameChangeUI(uint32_t user) {
  if (user >= 4) return kErrorInvalidParameter;
  return UserGetSigninState(user) ? ShowLiveUI() : kErrorNoSuchUser;
}

void Xam::NuiHudGetVersions(Guest g, uint32_t xam_version, uint32_t xtl_version) const {
  if (xam_version) g.Store64(xam_version, 0);
  if (xtl_version) g.Store64(xtl_version, 0);
}

uint64_t Xam::NuiIdentitySessionId() {
  if (!nui_session_id_) {
    std::random_device device;
    while (!nui_session_id_) nui_session_id_ = uint64_t(device()) << 32 | device();
  }
  return nui_session_id_;
}

uint32_t Xam::CachedUserFlags() const {
  return uint32_t(Console().country) << 8 | (Console().language << 25 & 0x3E000000u);
}

void Xam::WriteAccount(Guest g, uint32_t account) const {
  std::memset(g.host(account), 0, kAccountSize);
  for (size_t i = 0; i < std::min<size_t>(title_.gamertag.size(), 15); ++i) {
    g.Store16(account + 8 + 2 * uint32_t(i), uint16_t(uint8_t(title_.gamertag[i])));
  }
  g.Store32(account + 0x30, CachedUserFlags());
}

void Xam::WriteProfileEnumResult(Guest g, uint32_t at) const {
  // X_PROFILEENUMRESULT: the offline XUID, the account, device 1.
  g.Store64(at, title_.xuid);
  WriteAccount(g, at + 8);
  g.Store32(at + kProfileEnumResultSize - 4, 1);
}

uint32_t Xam::ProfileCreateEnumerator(Guest g, uint32_t handle_ptr) {
  if (!handle_ptr) return kErrorInvalidParameter;
  const uint32_t handle = CreateEnumerator(g, kProfileEnumResultSize, 1, {});
  if (!handle) return kErrorFunctionFailed;
  enumerators_.back().local_profile = true;
  g.Store32(handle_ptr, handle);
  return kErrorSuccess;
}

uint32_t Xam::ProfileEnumerate(Guest g, uint32_t handle, uint32_t buffer, uint32_t overlapped) {
  return Enumerate(g, handle, 0, buffer, kProfileEnumResultSize, 0, overlapped);
}

uint32_t Xam::ProfileFindAccount(Guest g, uint64_t xuid, uint32_t account, uint32_t device_ptr) const {
  if (!account) return kErrorInvalidParameter;
  std::memset(g.host(account), 0, kAccountSize);
  if (xuid != title_.xuid) return kErrorNoSuchUser;
  WriteAccount(g, account);
  if (device_ptr) g.Store32(device_ptr, 1);
  return kErrorSuccess;
}

uint32_t Xam::ProfileOpen(Guest g, uint64_t xuid, uint32_t mount_name, uint32_t content_data) {
  if (xuid != title_.xuid) return kErrorInvalidParameter;
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llX", static_cast<unsigned long long>(xuid));
  // Mounted once per name: a title opening its profile again finds it as it left it (no host file system call).
  char root[72] = {};
  if (mount_name) {
    for (size_t i = 0; i < 64 && g.Load8(mount_name + uint32_t(i)); ++i) root[i] = char(g.Load8(mount_name + uint32_t(i)));
  }
  if (!root[0]) std::snprintf(root, sizeof root, "User_%s", hex);
  if (std::find(profile_mounts_.begin(), profile_mounts_.end(), root) == profile_mounts_.end()) {
    if (profile_dir_.empty() || profile_dir_root_ != title_.content_root) {
      profile_dir_ = title_.content_root / "profile" / hex;
      profile_dir_root_ = title_.content_root;
      std::error_code error;
      std::filesystem::create_directories(profile_dir_, error);
    }
    Files().Mount(std::string("\\Device\\Content\\") + root, profile_dir_, false);
    Files().Link(std::string(root) + ":", std::string("\\Device\\Content\\") + root);
    profile_mounts_.emplace_back(root);
  }
  if (content_data) {
    g.Store32(content_data, 1);                // device
    g.Store32(content_data + 4, 0x10000);      // XContentType::kProfile
    std::memset(g.host(content_data + kContentFileNameOffset), 0, 42);
    std::memcpy(g.host(content_data + kContentFileNameOffset), hex, 16);
    g.Store32(content_data + 0x140, 0xFFFE07D1);  // the dashboard's title id
  }
  return kErrorSuccess;
}

uint32_t Xam::UserGetIndexFromXuid(Guest g, uint64_t xuid, uint32_t index_ptr) const {
  if (!index_ptr) return 0x80070057;  // E_INVALIDARG
  if (xuid != title_.xuid) return 0x80070525;  // HRESULT_FROM_WIN32(ERROR_NO_SUCH_USER)
  g.Store32(index_ptr, 0);
  return kErrorSuccess;
}

uint32_t Xam::UserGetSubscriptionType(Guest g, uint32_t user, uint32_t length_ptr, uint32_t payment_ptr) const {
  if (user >= 4 || !length_ptr || !payment_ptr) return 0x80070057;
  if (!UserGetSigninState(user)) return 0x80070525;
  g.Store32(length_ptr, 0);
  g.Store32(payment_ptr, 0);
  return kErrorSuccess;
}

uint32_t Xam::UserLogon(Guest g, uint32_t xuids, uint32_t overlapped) {
  if (!xuids) return kErrorInvalidParameter;
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::ProfileClose(Guest g, uint32_t mount_name) {
  const std::string root = GuestString(g, mount_name, 64);
  auto mount = std::find(profile_mounts_.begin(), profile_mounts_.end(), root);
  if (mount != profile_mounts_.end()) {
    profile_mounts_.erase(mount);
  } else {
    auto content = std::find(open_content_.begin(), open_content_.end(), root);
    if (content == open_content_.end()) return kErrorFunctionFailed;
    open_content_.erase(content);
  }
  Files().Unlink(root + ":");
  Files().Unmount("\\Device\\Content\\" + root);
  return kErrorSuccess;
}

uint32_t Xam::ProfileCreate(Guest g, uint32_t device_ptr, uint32_t creation_info_ptr) const {
  if (device_ptr) g.Store32(device_ptr, 1);
  if (creation_info_ptr) g.Store32(creation_info_ptr, 0);
  return kErrorInvalidParameter;
}

uint32_t Xam::ProfileGetCreationStatus(Guest g, uint32_t creation_info, uint32_t xuid_ptr) const {
  if (creation_info && xuid_ptr) g.Store64(xuid_ptr, g.Load64(creation_info + 0xA78));
  return kErrorSuccess;
}

uint32_t Xam::ProfileLoadAccountInfo(Guest g, uint64_t xuid, uint32_t account) const {
  if (!account || !xuid || xuid != title_.xuid) return kErrorInvalidParameter;
  WriteAccount(g, account);
  return kErrorSuccess;
}

uint32_t Xam::UserCreateTitlesPlayedEnumerator(Guest g, uint32_t user, uint32_t count, uint32_t size_ptr,
                                               uint32_t handle_ptr) {
  if (user >= 4 || !count || !size_ptr || !handle_ptr) return kErrorInvalidParameter;
  if (!UserGetSigninState(user)) return kErrorFunctionFailed;
  constexpr uint32_t kTitlePlayedSize = 0x28 + 64 * 2;
  g.Store32(size_ptr, count * kTitlePlayedSize);
  const uint32_t handle = CreateEnumerator(g, kTitlePlayedSize, count, {});
  if (!handle) return kErrorFunctionFailed;
  g.Store32(handle_ptr, handle);
  return kErrorSuccess;
}

uint32_t Xam::UserIsUnsafeProgrammingAllowed(Guest g, uint32_t user, uint32_t result_ptr) const {
  if (!result_ptr || (user != 0xFF && user >= 4)) return kErrorInvalidParameter;
  g.Store32(result_ptr, 1);
  return kErrorSuccess;
}

uint32_t Xam::NetworkStorageHasUserEnabledStorage(uint32_t user) const {
  if (user >= 4 || !UserGetSigninState(user)) return 0;
  if (user_preferences_ & 0x20) return 1;
  return NetworkStorageUserHasPrivilege(user) ? 1 : 0;
}

uint32_t Xam::UserNuiGetUserIndexForSignin(Guest g, uint32_t index_ptr) const {
  if (!index_ptr) return 0x80070057;
  g.Store32(index_ptr, 0);
  return 0;
}

namespace {
// The "written" bit of a preference mask (user_settings.h's GetUserPreferenceWrittenBit); ~0 for a mask that names no
// preference.
uint32_t PreferenceWrittenBit(uint32_t mask) {
  switch (mask) {
    case 0x001: case 0x020: case 0x040: case 0x080: case 0x100: case 0x200: return 0;
    case 0x006: return 0x004;
    case 0x018: return 0x010;
    default: return ~0u;
  }
}
}  // namespace

uint32_t Xam::UserReadUserPreference(Guest g, uint32_t user, uint32_t mask, uint32_t value_ptr, uint32_t overlapped) {
  if (user >= 4 || !value_ptr) return kErrorInvalidParameter;
  uint32_t result = kErrorSuccess, extended = 0;
  const uint32_t written = PreferenceWrittenBit(mask);
  if (!UserGetSigninState(user)) {
    result = kErrorFunctionFailed, extended = 0x80070525;
  } else if (written == ~0u) {
    result = kErrorFunctionFailed, extended = 0x80070057;
  } else {
    uint32_t value = user_preferences_ & mask;
    if (written && (value & written) != written) {
      extended = 1;  // S_FALSE: never written; the value stands as it is
    } else {
      value &= ~written;
    }
    g.Store32(value_ptr, value);
  }
  if (!overlapped) return result;
  MsgCompleteIORequest(g, overlapped, result, extended, 0);
  return kErrorIoPending;
}

uint32_t Xam::UserWriteUserPreference(Guest g, uint32_t user, uint32_t mask, uint32_t value, uint32_t overlapped) {
  if (user >= 4) return kErrorInvalidParameter;
  uint32_t result = kErrorSuccess, extended = 0;
  const uint32_t written = PreferenceWrittenBit(mask);
  if (!UserGetSigninState(user)) {
    result = kErrorFunctionFailed, extended = 0x80070525;
  } else if (written == ~0u || (mask | value) != mask || (value & written)) {
    result = kErrorFunctionFailed, extended = 0x80070057;
  } else {
    user_preferences_ = (user_preferences_ & ~mask) | value | written;
    constexpr uint32_t kNotifyProfileSettingChanged = 0x0000000E;
    Notify(kNotifyProfileSettingChanged, 1u << user);
  }
  if (!overlapped) return result;
  MsgCompleteIORequest(g, overlapped, result, extended, 0);
  return kErrorIoPending;
}

uint32_t Xam::GetCachedTitleName(Guest g, uint32_t title_id, uint32_t buffer, uint32_t length_ptr) const {
  const std::u16string empty;
  const std::u16string& name = title_id ? title_.title_name : empty;
  for (size_t i = 0; i < name.size(); ++i) g.Store16(buffer + 2 * uint32_t(i), uint16_t(name[i]));
  g.Store16(buffer + 2 * uint32_t(name.size()), 0);
  g.Store32(length_ptr, uint32_t(name.size()) + 1);
  return title_id ? 0 : kErrorInvalidParameter;
}

uint32_t Xam::LanguageLocaleFallbackString(Guest g, uint32_t language) {
  constexpr uint32_t kLanguages = 18;
  if (language >= kLanguages) return 0;
  if (!language_fallback_) {
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < kLanguages; ++i) bytes += 2 * uint32_t(std::char_traits<char16_t>::length(LanguageLocaleFallback(i)) + 1);
    const uint32_t at = Objects().AllocateObject(bytes);
    if (!at) return 0;
    uint32_t next = at;
    for (uint32_t i = 0; i < kLanguages; ++i) {
      language_fallback_at_[i] = next;
      const char16_t* name = LanguageLocaleFallback(i);
      do {
        g.Store16(next, uint16_t(*name));
        next += 2;
      } while (*name++);
    }
    language_fallback_ = at;
  }
  return language_fallback_at_[language];
}

uint32_t Xam::LanguageTypefacePatch(Guest g, uint32_t language) {
  static constexpr const char16_t* kPatches[3] = {u"file://media:/XenonSCLatin.xttp2", u"file://media:/XenonCLatin.xttp2",
                                                  u"file://media:/XenonJKLatin.xttp2"};
  if (!typeface_patch_at_[0]) {
    uint32_t bytes = 0;
    for (const char16_t* patch : kPatches) bytes += 2 * uint32_t(std::char_traits<char16_t>::length(patch) + 1);
    uint32_t next = Objects().AllocateObject(bytes);
    if (!next) return 0;
    for (uint32_t i = 0; i < 3; ++i) {
      typeface_patch_at_[i] = next;
      const char16_t* name = kPatches[i];
      do {
        g.Store16(next, uint16_t(*name));
        next += 2;
      } while (*name++);
    }
  }
  return typeface_patch_at_[language == 17 ? 0 : language == 8 ? 1 : 2];
}

uint32_t Xam::LiveHiveValue(Guest g, uint32_t name, uint32_t value, uint32_t size, uint32_t overlapped) {
  const uint32_t result = !name || !value || !size ? 0x80070057u : 0x80151802u;
  if (!overlapped) return result;
  CompleteEx(g, overlapped, result, result, 0xFFFFFFFF);
  return kErrorIoPending;
}

uint32_t Xam::QueryLiveHiveA(Guest g, uint32_t name, uint32_t value, uint32_t size, uint32_t overlapped) {
  const uint32_t extended = !name || !value || !size ? 0x80070057u : 0x80151802u;
  if (!overlapped) return extended;
  CompleteEx(g, overlapped, kErrorSuccess, extended, 0);
  return 0x8000000A;  // E_PENDING
}

void Xam::SmcSetTray(uint8_t state) {
  smc_tray_ = state;
  Notify(0x8000000D, state);  // XN_SYS_TRAYSTATECHANGED
}

void Xam::SmcMessage(Guest g, uint32_t message, uint32_t response) {
  if (!message) return;
  const uint8_t command = g.Load8(message), data = g.Load8(message + 1);
  auto answer = [&](std::initializer_list<uint8_t> bytes) {
    if (!response) return;
    g.Store8(response, command);
    uint32_t at = response + 1;
    for (const uint8_t b : bytes) g.Store8(at++, b);
  };
  switch (command) {
    case 0x07:  // QUERY_TEMP_SENSOR: 69.6, 69.9, 69.6, 69.9 C as Xenia stores them (t * 256: its low byte first)
      answer({0x99, 0x45, 0xE6, 0x45, 0x99, 0x45, 0xE6, 0x45});
      break;
    case 0x0A: answer({smc_tray_}); break;              // QUERY_TRAY
    case 0x0F: answer({0x5B}); break;                   // QUERY_AV_PACK: VGA (XGetAVPack 6)
    case 0x12: answer({65, 2, 1}); break;               // QUERY_SMC_VERSION
    case 0x16: case 0x95: answer({0x0F}); break;        // QUERY_IR_ADDRESS, SET_IR_ADDRESS: the Xbox 360 remote
    case 0x17: answer({0}); break;                      // QUERY_TILT_SENSOR: vertical
    case 0x89: smc_cpu_fan_ = uint8_t(data - 0x80); break;  // SET_FAN_SPEED_CPU
    case 0x94: smc_gpu_fan_ = uint8_t(data - 0x80); break;  // SET_FAN_SPEED_GPU
    case 0x8B: SmcSetTray(uint8_t((data & 0xF) % 5)); break;  // SET_DVD_TRAY
    case 0x8C: smc_power_led_ = uint16_t(data << 8 | g.Load8(message + 2)); break;  // SET_POWER_LED
    case 0x99: smc_leds_ = uint16_t(data << 8 | g.Load8(message + 2)); break;       // SET_LEDS
    default: break;
  }
}

uint32_t Xam::IptvServiceName(Guest g, uint32_t name_out) {
  constexpr uint32_t kNameBytes = 0x78;
  if (!iptv_name_) {
    const uint32_t at = Objects().AllocateObject(kNameBytes);
    if (!at) return 0x8007000E;
    std::memset(g.host(at), 0, kNameBytes);
    iptv_name_ = at;
  }
  g.Store32(name_out, iptv_name_);
  return 0;
}

uint32_t Xam::UserGetDeviceContext(Guest g, uint32_t user, uint32_t context_out) {
  g.Store32(context_out, 0);
  return !user || (user & 0xFF) == 0xFF ? 0 : 0x80070000 | kErrorDeviceNotConnected;
}

uint32_t Xam::NotifyCreateListener(Guest g, uint64_t mask, uint32_t) {
  const uint32_t object = Objects().AllocatePlainObject(16, close_hook_);
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

uint32_t Xam::UserGetSigninState(uint32_t user) const { return user == 0 ? 1 : 0; }  // signed in locally

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

namespace {
// The integer profile settings a new profile holds, as a console answers them before a title writes any: those
// that are not 0 (vibration on, a default voice volume, the gamer card's defaults, auto aim on).
uint32_t DefaultProfileInt(uint32_t id) {
  switch (id) {
    case 0x10040003: return 3;           // XPROFILE_OPTION_CONTROLLER_VIBRATION: on
    case 0x10040006: return 0xFA;        // XPROFILE_GAMERCARD_CRED
    case 0x1004000E: return 0x64;        // XPROFILE_OPTION_VOICE_VOLUME
    case 0x10040012: return 1;           // XPROFILE_GAMERCARD_TITLES_PLAYED
    case 0x1004001D: return 0xFFFF0000;  // preferred color 1
    case 0x1004001E: return 0xFF00FF00;  // preferred color 2
    case 0x10040022: return 1;           // XPROFILE_GAMER_ACTION_AUTO_AIM
    default: return 0;
  }
}
}  // namespace

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
      const auto stored = FindSetting(id);
      g.Store32(at + 0, stored != settings_.end() ? 2 : 1);  // from: title-written or default
      if (xuid_count) g.Store64(at + 8, title_.xuid);
      else g.Store32(at + 8, user);
      g.Store32(at + 16, id);
      g.Store8(at + 24, uint8_t(id >> 28));  // data type
      const uint32_t type = id >> 28, bytes = (id >> 16) & 0xFFF;
      if (stored != settings_.end()) std::memcpy(g.host(at + 24), stored->data.data(), 16);
      else if (type == 1) g.Store32(at + 32, DefaultProfileInt(id));  // never written: the console's default
      if (type == 4 || type == 6) {
        g.Store8(at + 24, uint8_t(type));
        g.Store32(at + 36, data);
        // A written setting answers with the length written (a title's options blob is shorter than its maximum);
        // one never written with its maximum, zeroed.
        uint32_t length = bytes;
        if (stored != settings_.end() && stored->data.size() >= 16) {
          length = uint32_t(std::min<size_t>(bytes, stored->data.size() - 16));
          std::memcpy(g.host(data), stored->data.data() + 16, length);
        }
        g.Store32(at + 32, length);
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
    PutSetting(std::move(s));
  }
  return Finish(g, overlapped, kErrorSuccess);
}

void Xam::PutSetting(Setting s) {
  if (s.id >> 28 == 6 && !title_.profile_root.empty()) {
    std::error_code error;
    std::filesystem::create_directories(title_.profile_root, error);
    char file[16];
    std::snprintf(file, sizeof file, "%08X", s.id);
    std::ofstream out(title_.profile_root / file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(s.data.data() + 16), std::streamsize(s.data.size() - 16));
  }
  auto it = std::find_if(settings_.begin(), settings_.end(), [&](const Setting& x) { return x.id == s.id; });
  if (it != settings_.end()) *it = std::move(s);
  else settings_.push_back(std::move(s));
}

void Xam::CompleteEx(Guest g, uint32_t overlapped, uint32_t result, uint32_t extended, uint32_t length) {
  g.Store32(overlapped + 0x00, result);
  g.Store32(overlapped + 0x04, length);
  g.Store32(overlapped + 0x18, extended);
  const uint32_t event = g.Load32(overlapped + 0x0C);
  if (event) Objects().NtSetEvent(g, event, 0);
}

// --- step 12, the calls Xenia stubs too: avatars (Xenia's xam_avatar.cc) ---

namespace {
constexpr uint32_t kAvatarEditorTitleId = 0x584D07D1, kAvatarInfoSetting = 0x63E80044, kAvatarInfoSize = 0x3E8;
constexpr uint32_t kEFail = 0x80004005, kEInvalidArgument = 0x80070057, kENoSuchUser = 0x80070525;
}  // namespace

uint32_t Xam::MediaVerificationFailedBlocks(Guest g, uint32_t information) {
  if (!information) return kErrorSuccess;
  if (g.Load32(information) != 0x18) return 8;  // ERROR_NOT_ENOUGH_MEMORY
  // Failed reads, failed hashes, blocks checked, total blocks: 0; complete.
  std::memset(g.host(information + 4), 0, 16);
  g.Store32(information + 20, 1);
  return kErrorSuccess;
}

uint32_t Xam::XAuthStartup(Guest g, uint32_t settings) {
  return !settings || g.Load32(settings) != 8 ? 0x80158401 : 0x80158406;
}

uint32_t Xam::AvatarWearNow(Guest g, uint32_t overlapped) {
  if (title_.title_id == kAvatarEditorTitleId) return kErrorSuccess;
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::AvatarGetAssets(Guest g, uint32_t overlapped) {
  if (!overlapped) return kErrorSuccess;
  CompleteEx(g, overlapped, kErrorFunctionFailed, kEFail, 0);
  return kErrorIoPending;
}

uint32_t Xam::AvatarGetAssetsResultSize(Guest g, uint32_t result_size_ptr, uint32_t gpu_size_ptr) {
  if (result_size_ptr) g.Store32(result_size_ptr, 0);
  if (gpu_size_ptr) g.Store32(gpu_size_ptr, 0);
  return kErrorSuccess;
}

void Xam::AvatarGetInstalledAssetPackageDescription(Guest g, uint32_t asset_id, uint32_t content_data) {
  std::memset(g.host(content_data), 0, kContentAggregateSize);
  g.Store32(content_data + 4, 0x9000);  // avatar item
  const uint32_t title_id = g.Load32(asset_id + 12);
  // The 32 hexadecimal digits of the id, the second id and the title id: the 16 bytes as they lie, big-endian.
  static constexpr char kHex[] = "0123456789ABCDEF";
  const uint8_t* id = g.host(asset_id);
  char* name = reinterpret_cast<char*>(g.host(content_data + kContentFileNameOffset));
  for (int i = 0; i < 16; ++i) name[2 * i] = kHex[id[i] >> 4], name[2 * i + 1] = kHex[id[i] & 15];
  g.Store32(content_data + 0x140, title_id);
}

uint32_t Xam::AvatarGetInstrumentation(Guest g, uint32_t value_ptr) {
  if (value_ptr) g.Store32(value_ptr, 0);
  return 1;
}

uint32_t Xam::AvatarGetManifestLocalUser(Guest g, uint32_t user, uint32_t metadata, uint32_t overlapped) {
  uint32_t result = kErrorSuccess, extended = kErrorSuccess;
  if (user >= 4 || !metadata) {
    result = kErrorInvalidParameter, extended = kEInvalidArgument;
  } else if (user != 0) {
    result = kErrorFunctionFailed, extended = kENoSuchUser;
  } else {
    const auto stored = FindSetting(kAvatarInfoSetting);
    if (stored == settings_.end() || stored->data.size() <= 16) {
      result = kErrorFunctionFailed, extended = kEFail;
    } else {
      std::memcpy(g.host(metadata), stored->data.data() + 16, std::min<size_t>(stored->data.size() - 16, kAvatarInfoSize));
    }
  }
  if (!overlapped) return result == kErrorSuccess ? result : extended;
  CompleteEx(g, overlapped, result, extended, 0);
  return kErrorIoPending;
}

uint32_t Xam::AvatarSetManifest(Guest g, uint32_t user, uint32_t info, uint32_t overlapped) {
  uint32_t result = kErrorSuccess, extended = kErrorSuccess;
  if (user != 0) {
    result = kErrorFunctionFailed, extended = kENoSuchUser;
  } else {
    // The kept setting's bytes rewritten in place when it has its full size (no host allocation), else a new one.
    auto kept = std::find_if(settings_.begin(), settings_.end(), [](const Setting& x) { return x.id == kAvatarInfoSetting; });
    Setting s{kAvatarInfoSetting, {}};
    if (kept != settings_.end() && kept->data.size() == 16 + kAvatarInfoSize) s.data = std::move(kept->data);
    else s.data.assign(16 + kAvatarInfoSize, 0);
    s.data[0] = 6;  // binary
    for (int i = 0; i < 4; ++i) s.data[8 + i] = uint8_t(kAvatarInfoSize >> (24 - 8 * i));
    if (info) std::memcpy(s.data.data() + 16, g.host(info), kAvatarInfoSize);
    else std::memset(s.data.data() + 16, 0, kAvatarInfoSize);
    PutSetting(std::move(s));
  }
  if (!overlapped) return result == kErrorSuccess ? result : extended;
  CompleteEx(g, overlapped, result, extended, 0);
  return kErrorIoPending;
}

std::vector<Xam::Setting>::const_iterator Xam::FindSetting(uint32_t id) {
  auto it = std::find_if(settings_.begin(), settings_.end(), [&](const Setting& s) { return s.id == id; });
  if (it != settings_.end() || id >> 28 != 6 || title_.profile_root.empty()) return it;
  // A binary setting written in an earlier run: its bytes from the profile folder, behind the 16 bytes of
  // X_USER_DATA a write keeps (the type, then the size at +8, big-endian).
  char file[16];
  std::snprintf(file, sizeof file, "%08X", id);
  std::ifstream in(title_.profile_root / file, std::ios::binary);
  if (!in) return settings_.end();
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Setting s{id, std::vector<uint8_t>(16, 0)};
  s.data[0] = 6;
  const uint32_t size = uint32_t(bytes.size());
  for (int i = 0; i < 4; ++i) s.data[8 + i] = uint8_t(size >> (24 - 8 * i));
  s.data.insert(s.data.end(), bytes.begin(), bytes.end());
  settings_.push_back(std::move(s));
  return settings_.end() - 1;
}

uint32_t Xam::SessionCreateHandle(Guest g, uint32_t handle_ptr) {
  const uint32_t object = Objects().AllocatePlainObject(16, ObjectManager::kNoCloseHook);
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

uint32_t Xam::ShowSigninUIEx(Guest g, uint32_t users, uint32_t flags, uint32_t overlapped) {
  const uint32_t result = ShowSigninUI(users, flags);
  if (!overlapped) return result;
  CompleteEx(g, overlapped, result, result, result ? 0xFFFFFFFF : 0);
  return kErrorIoPending;
}

uint32_t Xam::ShowMarketplaceUIEx(uint32_t user, uint32_t entry_point) {
  if (user >= 4 || entry_point >= 25) return kErrorInvalidParameter;
  if (!UserGetSigninState(user)) return kErrorNoSuchUser;
  return ShowLiveUI();
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
  // The overlapped result says only success or failure; the extended error says why, as an HRESULT.
  if (!overlapped) return result;
  Complete(g, overlapped, result == kErrorSuccess ? kErrorSuccess : kErrorFunctionFailed, 0);
  g.Store32(overlapped + 0x18, result ? 0x80070000u | result : 0);
  return kErrorIoPending;
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

uint32_t Xam::ShowMarketplaceDownloadItemsUI(Guest g, uint32_t user, uint32_t result_ptr, uint32_t overlapped) {
  if (user >= 4) return kErrorInvalidParameter;
  ShowLiveUI();
  if (result_ptr) g.Store32(result_ptr, 0);
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
  const uint32_t object = Objects().AllocatePlainObject(16, close_hook_);
  if (!object) return 0;
  std::memset(g.host(object), 0, 16);
  enumerators_.push_back({object, item_size, std::max<uint32_t>(1, per_call), 0, std::move(items)});
  return Objects().OpenHandle(object);
}

Xam::Enumerator* Xam::FindEnumerator(uint32_t handle) {
  const uint32_t object = Objects().Lookup(handle);
  if (!object) return nullptr;
  for (auto e = enumerators_.rbegin(); e != enumerators_.rend(); ++e) {
    if (e->object == object) return &*e;
  }
  return nullptr;
}

uint32_t Xam::Enumerate(Guest g, uint32_t handle, uint32_t, uint32_t buffer, uint32_t length, uint32_t items_ptr, uint32_t overlapped) {
  Enumerator* e = FindEnumerator(handle);
  if (!e) return kErrorInvalidParameter;
  if (items_ptr) g.Store32(items_ptr, 0);
  if (length < e->item_size) return Finish(g, overlapped, kErrorInsufficientBuffer, 0, true);
  uint32_t n = 0;
  const size_t count = e->local_profile ? 1 : e->items.size();
  while (e->next < count && n < e->per_call && (n + 1) * e->item_size <= length) {
    const uint32_t at = buffer + n * e->item_size;
    if (e->local_profile) {
      WriteProfileEnumResult(g, at);
    } else {
      std::memcpy(g.host(at), e->items[e->next].data(), std::min<size_t>(e->item_size, e->items[e->next].size()));
    }
    ++e->next;
    ++n;
  }
  if (items_ptr) g.Store32(items_ptr, n);
  return Finish(g, overlapped, n ? kErrorSuccess : kErrorNoMoreFiles, n, true);
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
  // A package is a folder of its own under its type's folder. A name that is empty, or only dots and spaces, would
  // name the type's folder itself or its parent: creating "always" or deleting such a package would then remove
  // every package of the type (it happened: a title asked for a package with an empty name and all its saved games
  // went). No such package exists: an empty path, which every caller refuses.
  if (name.find_first_not_of(". ") == std::string::npos) return {};
  return title_.content_root / type / name;
}

// The package's XCONTENT_DATA as the title gave it (its display name above all), kept beside the packages as the
// runtime keeps it: Headers/<type>/<file name>.header, an XCONTENT_AGGREGATE_DATA.
std::filesystem::path Xam::ContentHeaderPath(const std::filesystem::path& package) const {
  return title_.content_root / "Headers" / package.parent_path().filename() / (package.filename().string() + ".header");
}

uint32_t Xam::ContentCreate(Guest g, uint32_t user, uint32_t root_name, uint32_t content_data, uint32_t flags, uint32_t disposition_ptr,
                            uint32_t license_ptr, uint32_t overlapped) {
  (void)user;
  if (!content_data || !root_name || title_.content_root.empty()) return Finish(g, overlapped, kErrorInvalidParameter, 0, true);
  const std::filesystem::path path = ContentPath(g, content_data);
  if (path.empty()) return Finish(g, overlapped, kErrorInvalidParameter, 0, true);
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  // XCONTENTFLAG creation modes: create new, create always, open existing, open always, truncate existing.
  const uint32_t mode = flags & 0xF;
  uint32_t result = kErrorSuccess, disposition = exists ? 2 : 1;  // opened / created
  if ((mode == 1 && exists)) result = kErrorAlreadyExists;
  else if ((mode == 3 || mode == 5) && !exists) result = kErrorFileNotFound;
  else {
    if (exists && (mode == 2 || mode == 5)) std::filesystem::remove_all(path, error);
    if (!exists || mode == 2 || mode == 5) {
      std::vector<uint8_t> header(kContentAggregateSize, 0);
      std::memcpy(header.data(), g.host(content_data), kContentDataSize);
      for (int i = 0; i < 4; ++i) header[0x140 + i] = uint8_t(title_.title_id >> (24 - 8 * i));
      const std::filesystem::path header_path = ContentHeaderPath(path);
      std::filesystem::create_directories(header_path.parent_path(), error);
      std::ofstream(header_path, std::ios::binary | std::ios::trunc)
          .write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
    }
    std::filesystem::create_directories(path, error);
    const std::string root = GuestString(g, root_name, 64);
    Files().Mount("\\Device\\Content\\" + root, path, false);
    Files().Link(root + ":", "\\Device\\Content\\" + root);
    open_content_.push_back(root);
  }
  if (disposition_ptr) g.Store32(disposition_ptr, result ? 0 : disposition);
  if (license_ptr) g.Store32(license_ptr, 0);
  // Through an overlapped structure the disposition is also its length (a title passing no disposition pointer reads
  // it there: 0 means neither opened nor created).
  return Finish(g, overlapped, result, result ? 0 : disposition, true);
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
  if (path.empty() || !std::filesystem::exists(path, error)) return Finish(g, overlapped, kErrorFileNotFound);
  std::filesystem::remove_all(path, error);
  std::filesystem::remove(ContentHeaderPath(path), error);
  return Finish(g, overlapped, kErrorSuccess);
}

namespace {

// `size` bytes of a host file from `offset` (a package's header), a wide path on Windows (the
// content root may hold any character). False when the file is missing or shorter.
bool ReadHostFile(const std::filesystem::path& path, long offset, void* buffer, size_t size) {
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (_wfopen_s(&file, path.c_str(), L"rb")) file = nullptr;
#else
  file = std::fopen(path.c_str(), "rb");
#endif
  if (!file) return false;
  const bool read = std::fseek(file, offset, SEEK_SET) == 0 && std::fread(buffer, 1, size, file) == size;
  std::fclose(file);
  return read;
}

}  // namespace

std::vector<std::vector<uint8_t>> Xam::ContentItems(uint32_t device_id, uint32_t content_type, uint32_t item_size) const {
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
    std::vector<uint8_t> item(item_size, 0);
    const uint32_t device = device_id ? device_id : 1;
    const uint8_t be_device[4] = {uint8_t(device >> 24), uint8_t(device >> 16), uint8_t(device >> 8), uint8_t(device)};
    const uint8_t be_type[4] = {uint8_t(content_type >> 24), uint8_t(content_type >> 16), uint8_t(content_type >> 8), uint8_t(content_type)};
    std::memcpy(item.data(), be_device, 4);
    std::memcpy(item.data() + 4, be_type, 4);
    // The display name the title gave (its header), else the file name (UTF-16BE).
    std::ifstream header(ContentHeaderPath(title_.content_root / type / name), std::ios::binary);
    if (!header || !header.seekg(8).read(reinterpret_cast<char*>(item.data() + 8), kContentFileNameOffset - 8)) {
      std::fill(item.begin() + 8, item.begin() + kContentFileNameOffset, uint8_t(0));
      for (size_t i = 0; i < name.size() && i < 127; ++i) item[8 + 2 * i + 1] = uint8_t(name[i]);
    }
    std::memcpy(item.data() + kContentFileNameOffset, name.data(), std::min<size_t>(name.size(), 41));
    items.push_back(std::move(item));
  }
  return items;
}

uint32_t Xam::ContentCreateEnumerator(Guest g, uint32_t, uint32_t device_id, uint32_t content_type, uint32_t, uint32_t per_call,
                                      uint32_t size_ptr, uint32_t handle_out) {
  if (!handle_out) return kErrorInvalidParameter;
  std::vector<std::vector<uint8_t>> items = ContentItems(device_id, content_type, kContentDataSize);
  if (size_ptr) g.Store32(size_ptr, kContentDataSize * std::max<uint32_t>(1, per_call));
  const uint32_t handle = CreateEnumerator(g, kContentDataSize, per_call, std::move(items));
  if (!handle) return kErrorInvalidParameter;
  g.Store32(handle_out, handle);
  return kErrorSuccess;
}

uint32_t Xam::ContentCreateEnumeratorInternal(Guest g, uint32_t device_id, uint32_t content_type, uint32_t per_call,
                                              uint32_t size_ptr, uint32_t handle_out) {
  constexpr uint32_t kEInvalidArg = 0x80070057;
  if (device_id > 2 || !handle_out) {
    if (size_ptr) g.Store32(size_ptr, 0);
    return kEInvalidArg;
  }
  if (size_ptr) g.Store32(size_ptr, kContentDataSize * per_call);
  std::vector<std::vector<uint8_t>> items;
  if (device_id != 2) items = ContentItems(device_id, content_type, kContentDataSize);  // the disc holds none
  const uint32_t handle = CreateEnumerator(g, kContentDataSize, per_call, std::move(items));
  if (!handle) return kEInvalidArg;
  g.Store32(handle_out, handle);
  return kErrorSuccess;
}

uint32_t Xam::ContentGetMetaDataInternal(Guest g, uint32_t content_data, uint32_t metadata, uint32_t overlapped) {
  // XCONTENT_METADATA (Xenia's XContentMetadata, packed, 0x93D6 bytes): type 0x00, version 0x04, execution info 0x10
  // (title id at +0x0C), data file count 0x59, volume type 0x65 (0: STFS), display names 0xCD (9 x 256 bytes).
  constexpr uint32_t kSize = 0x93D6, kTitleId = 0x1C, kDataFileCount = 0x59, kDisplayName = 0xCD;
  if (!content_data || title_.content_root.empty()) return kErrorFunctionFailed;
  const std::filesystem::path path = ContentPath(g, content_data);
  std::error_code error;
  if (path.empty() || !std::filesystem::is_directory(path, error)) return kErrorFunctionFailed;
  std::memset(g.host(metadata), 0, kSize);
  uint8_t header[kContentAggregateSize];
  if (ReadHostFile(ContentHeaderPath(path), 0, header, sizeof header)) {
    std::memcpy(g.host(metadata), header + 4, 4);  // the type, big-endian as stored
    g.Store32(metadata + 4, 2);
    g.Store32(metadata + kDataFileCount, 1);
    std::memcpy(g.host(metadata + kTitleId), header + 0x140, 4);
    std::memcpy(g.host(metadata + kDisplayName), header + 8, 0xFE);
  }
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::ReadString(Guest g, uint64_t id, uint32_t buffer, uint32_t size_ptr, uint32_t overlapped) {
  if (!buffer || id == 0xFFFF) return 0x80070057;  // E_INVALIDARG
  if (!size_ptr) return 0x8007007A;                // ERROR_INSUFFICIENT_BUFFER
  if (g.Load32(size_ptr)) g.Store16(buffer, 0);
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::ContentAggregateCreateEnumerator(Guest g, uint32_t device_id, uint32_t content_type, uint32_t handle_out) {
  if (!handle_out || (device_id && device_id != 1)) return 0x80070057;  // E_INVALIDARG: no other device to look at
  // XCONTENT_AGGREGATE_DATA: the XCONTENT_DATA, then the owner's XUID (8-aligned) and the title id.
  std::vector<std::vector<uint8_t>> items = ContentItems(device_id, content_type, kContentAggregateSize);
  for (auto& item : items) {
    for (int i = 0; i < 8; ++i) item[0x138 + i] = uint8_t(title_.xuid >> (56 - 8 * i));
    for (int i = 0; i < 4; ++i) item[0x140 + i] = uint8_t(title_.title_id >> (24 - 8 * i));
  }
  const uint32_t handle = CreateEnumerator(g, kContentAggregateSize, 1, std::move(items));
  if (!handle) return 0x80070057;
  g.Store32(handle_out, handle);
  return kErrorSuccess;
}

uint32_t Xam::ContentSetThumbnail(Guest g, uint32_t content_data, uint32_t buffer, uint32_t size, uint32_t overlapped) {
  if (!content_data || title_.content_root.empty()) return Finish(g, overlapped, kErrorInvalidParameter);
  const std::filesystem::path path = ContentPath(g, content_data);
  std::error_code error;
  if (path.empty() || !std::filesystem::is_directory(path, error)) return Finish(g, overlapped, kErrorFileNotFound);
  std::ofstream stream(path.parent_path() / (path.filename().string() + ".png"), std::ios::binary | std::ios::trunc);
  if (size) stream.write(reinterpret_cast<const char*>(g.host(buffer)), size);
  return Finish(g, overlapped, stream ? kErrorSuccess : kErrorAccessDenied);
}

uint32_t Xam::ContentGetThumbnail(Guest g, uint32_t content_data, uint32_t buffer, uint32_t size_ptr, uint32_t overlapped) {
  if (!content_data || !size_ptr || title_.content_root.empty()) return Finish(g, overlapped, kErrorInvalidParameter);
  const std::filesystem::path path = ContentPath(g, content_data);
  std::error_code error;
  if (path.empty() || !std::filesystem::is_directory(path, error)) return Finish(g, overlapped, kErrorFileNotFound);
  std::ifstream stream(path.parent_path() / (path.filename().string() + ".png"), std::ios::binary);
  if (!stream) return Finish(g, overlapped, kErrorFileNotFound);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  const uint32_t room = g.Load32(size_ptr);
  g.Store32(size_ptr, uint32_t(bytes.size()));
  if (!buffer) return Finish(g, overlapped, kErrorSuccess);
  if (room < bytes.size()) return Finish(g, overlapped, kErrorInsufficientBuffer);
  if (!bytes.empty()) std::memcpy(g.host(buffer), bytes.data(), bytes.size());
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::ContentResolve(Guest g, uint32_t content_data, uint32_t buffer, uint32_t size) {
  if (!content_data || !buffer || title_.content_root.empty()) return kErrorInvalidParameter;
  const std::filesystem::path path = ContentPath(g, content_data);
  std::error_code error;
  if (path.empty() || !std::filesystem::is_directory(path, error)) return kErrorFileNotFound;
  const std::string device = "\\Device\\Content\\" + path.parent_path().filename().string() + "\\" + path.filename().string();
  if (size < device.size() + 1) return kErrorInsufficientBuffer;
  Files().Mount(device, path, false);
  std::memcpy(g.host(buffer), device.c_str(), device.size() + 1);
  return kErrorSuccess;
}

uint32_t Xam::ContentGetAttributesInternal(Guest g, uint32_t content_data, uint32_t attributes, uint32_t overlapped) {
  if (!content_data || !attributes || title_.content_root.empty()) return kErrorFunctionFailed;
  const std::filesystem::path path = ContentPath(g, content_data);
  uint64_t creation = 0, access = 0, write = 0, size = 0;
  bool directory = false;
  if (path.empty() || !HostFileStat(path, creation, access, write, size, directory) || !directory) return kErrorFunctionFailed;
  // X_FILE_ATTRIBUTE_DATA: attributes, three FILETIMEs (low word, then high word, as the file calls store them), size.
  g.Store32(attributes, 0);
  const uint64_t times[3] = {creation, access, write};
  for (uint32_t i = 0; i < 3; ++i) {
    g.Store32(attributes + 4 + 8 * i, uint32_t(times[i]));
    g.Store32(attributes + 8 + 8 * i, uint32_t(times[i] >> 32));
  }
  g.Store32(attributes + 28, 0);
  g.Store32(attributes + 32, 0);
  return Finish(g, overlapped, kErrorSuccess);
}

uint32_t Xam::ContentGetDeviceVolumePath(Guest g, uint32_t device_id, uint32_t path, uint32_t size) {
  if (device_id != 1) return kErrorFunctionFailed;
  static constexpr char kVolume[] = "hdd0\\";
  if (!size) return kErrorSuccess;
  const uint32_t length = std::min<uint32_t>(sizeof kVolume - 1, size - 1);
  std::memcpy(g.host(path), kVolume, length);
  g.Store8(path + length, 0);
  return kErrorSuccess;
}

uint32_t Xam::ContentGetLocalizedDeviceData(Guest g, uint32_t device_id, uint32_t data) {
  const uint32_t result = ContentGetDeviceData(g, device_id, data);
  if (result || device_id != 2) return result;
  static constexpr char16_t kName[] = u"Optical Drive";
  for (uint32_t i = 0; i < sizeof kName / 2; ++i) g.Store16(data + 24 + 2 * i, uint16_t(kName[i]));
  return kErrorSuccess;
}

uint32_t Xam::ContentGetLocalizedString(Guest g, uint32_t metadata, uint32_t name, uint32_t name_size, uint32_t description,
                                        uint32_t description_size) const {
  if (!metadata) return kErrorInvalidParameter;
  // XCONTENT_METADATA: version 0x04; display names 0xCD and descriptions 0x9CD (9 languages x 128 characters), the
  // display names and descriptions of languages 10-12 at 0x50D6 and 0x90D6 (version 2).
  const uint32_t language = Console().language, version = g.Load32(metadata + 4);
  auto slot = [&](uint32_t lang_id, uint32_t v1, uint32_t v2) -> uint32_t {
    if (lang_id >= 12) lang_id = 0;  // English
    if (lang_id < 9) return metadata + v1 + lang_id * 0x100;
    return version >= 2 ? metadata + v2 + (lang_id - 9) * 0x100 : 0;
  };
  // A string of the slot (big-endian UTF-16, up to 128 characters), truncated to `size` characters and terminated.
  auto copy = [&](uint32_t from, uint32_t to, uint32_t size) {
    if (!to || !size) return;
    uint32_t n = 0;
    while (from && n < 128 && n + 1 < size && g.Load16(from + 2 * n)) {
      g.Store16(to + 2 * n, g.Load16(from + 2 * n));
      ++n;
    }
    g.Store16(to + 2 * n, 0);
  };
  // Xenia reads the display name of language 0 (none) from slot 1, its description from the English slot.
  copy(slot(language ? language - 1 : 1, 0xCD, 0x50D6), name, name_size);
  copy(slot(language - 1, 0x9CD, 0x90D6), description, description_size);
  return kErrorSuccess;
}

uint32_t Xam::ContentResolveInternal(Guest g, uint32_t content_data, uint32_t path, uint32_t size, uint32_t create_directory,
                                     uint32_t root_name, uint32_t overlapped) {
  if (!content_data) return Finish(g, overlapped, kErrorInvalidParameter);
  const uint32_t device = g.Load32(content_data);
  if ((device != 1 && device != 2) || (root_name && device != 1)) return Finish(g, overlapped, kErrorInvalidParameter);
  // A package already resolved (mounted on its folder under the current content root), asked again with no folder to
  // make and no root name: its device path written from the guest bytes, no host path built (ContentPath's file name
  // rules: at most 42 characters, '/', '\\' and ':' as '_', a name of only dots and spaces refused).
  if (device == 1 && !create_directory && !root_name && !title_.content_root.empty()) {
    static constexpr char kPrefix[] = "\\Device\\Content\\", kHex[] = "0123456789ABCDEF";
    char text[sizeof kPrefix + 8 + 1 + 42];
    std::memcpy(text, kPrefix, sizeof kPrefix - 1);
    uint32_t n = sizeof kPrefix - 1;
    const uint32_t type = g.Load32(content_data + 4);
    for (int shift = 28; shift >= 0; shift -= 4) text[n++] = kHex[(type >> shift) & 15];
    text[n++] = '\\';
    const uint32_t name_at = n;
    bool ascii = true, only_dots = true;
    for (uint32_t i = 0; i < 42; ++i) {
      char ch = char(g.Load8(content_data + kContentFileNameOffset + i));
      if (!ch) break;
      if (ch == '/' || ch == '\\' || ch == ':') ch = '_';
      ascii &= static_cast<unsigned char>(ch) < 0x80;
      only_dots &= ch == '.' || ch == ' ';
      text[n++] = ch;
    }
    if (only_dots) return Finish(g, overlapped, kErrorInvalidParameter);
    const std::string_view resolved(text, n);
    if (ascii && Files().IsMountedOn(resolved, title_.content_root, resolved.substr(name_at - 9, 8), resolved.substr(name_at))) {
      if (path && size) {
        const uint32_t length = std::min<uint32_t>(n, size - 1);
        std::memcpy(g.host(path), text, length);
        g.Store8(path + length, 0);
      }
      return Finish(g, overlapped, kErrorSuccess);
    }
  }
  uint32_t result = kErrorSuccess;
  std::string resolved;
  if (device == 1) {
    const std::filesystem::path folder = ContentPath(g, content_data);
    if (folder.empty() || title_.content_root.empty()) return Finish(g, overlapped, kErrorInvalidParameter);
    if (create_directory) {
      std::error_code error;
      std::filesystem::create_directories(folder, error);
    }
    resolved = "\\Device\\Content\\" + folder.parent_path().filename().string() + "\\" + folder.filename().string();
    Files().Mount(resolved, folder, false);
    if (root_name) {
      const std::string root = GuestString(g, root_name, 64) + ":";
      if (Files().HasLink(root)) {
        result = kErrorAlreadyExists;
      } else {
        Files().Link(root, resolved);
      }
      resolved = root;
    }
  } else {
    // The disc: the path Xenia gives (the saved game's owner, the running title).
    char text[160];
    const uint32_t type = g.Load32(content_data + 4);
    std::snprintf(text, sizeof text, "D:\\content\\%016llX\\%08X\\%08X\\%s",
                  static_cast<unsigned long long>(type == 1 ? title_.xuid : 0), title_.title_id, type,
                  GuestString(g, content_data + kContentFileNameOffset, 42).c_str());
    resolved = text;
  }
  if (path && size) {
    const uint32_t length = std::min<uint32_t>(uint32_t(resolved.size()), size - 1);
    std::memcpy(g.host(path), resolved.data(), length);
    g.Store8(path + length, 0);
  }
  return Finish(g, overlapped, result);
}

uint32_t Xam::ReadTile(Guest g, uint32_t user, uint64_t item_id, uint32_t buffer, uint32_t size_ptr, uint32_t overlapped) {
  if ((user >= 4 || !UserGetSigninState(user)) && item_id != title_.xuid) return kErrorInvalidParameter;
  if (!size_ptr) return kErrorInvalidParameter;
  g.Store32(size_ptr, 0);
  const uint32_t result = buffer ? kErrorFileNotFound : kErrorInsufficientBuffer;
  if (!overlapped) return result;
  CompleteEx(g, overlapped, result, 0x80070000u | result, 0);
  return kErrorIoPending;
}

uint32_t Xam::ContentCreateDeviceEnumerator(Guest g, uint32_t per_call, uint32_t size_ptr, uint32_t handle_out) {
  if (!handle_out) return kErrorInvalidParameter;
  constexpr uint32_t kDeviceData = 80;
  if (size_ptr) g.Store32(size_ptr, kDeviceData * per_call);
  // XDEVICE_DATA of the hard drive, written straight into the item (a guest view whose address 0 is its first byte).
  std::vector<uint8_t> item(kDeviceData);
  ContentGetDeviceData(Guest{item.data()}, 1, 0);
  const uint32_t handle = CreateEnumerator(g, kDeviceData, per_call, {std::move(item)});
  if (!handle) return kErrorFunctionFailed;
  g.Store32(handle_out, handle);
  return kErrorSuccess;
}

uint32_t Xam::ContentGetDeviceName(Guest g, uint32_t device_id, uint32_t buffer, uint32_t capacity) {
  if (device_id != 1 && device_id != 2) return kErrorDeviceNotConnected;
  const char* label = device_id == 1 ? "Hard Drive" : "Disc";
  const uint32_t length = uint32_t(std::strlen(label));
  if (capacity < length + 1) return kErrorInsufficientBuffer;
  for (uint32_t i = 0; i <= length; ++i) g.Store16(buffer + 2 * i, uint16_t(label[i]));
  return kErrorSuccess;
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

bool Xam::Connected(uint32_t user) const {
  if (user > 3) return false;
  return host_.controller_connected ? host_.controller_connected(user) : user == 0;
}

void Xam::Sample(uint32_t user, std::chrono::steady_clock::time_point at) {
  GamepadState pad;
  if (host_.controller_state) host_.controller_state(user, pad);
  // The player's pad family, asked after its state (the host learns of a new pad while reading it): a change rebuilds
  // the mapper with that family's profile.
  if (host_.controller_family) {
    const ControllerFamily family = host_.controller_family(user);
    if (family != pads_[user].family) {
      pads_[user].family = family;
      pads_[user].mapper = MapperFor(user, family);
    }
  }
  pad = pads_[user].mapper.Map(pad);
  // A replay takes the first controller's place once it starts (the controller left out).
  if (user == 0 && replaying_ && at >= replay_start_) pad = replay_.StateAt(at - replay_start_);
  // The input script's presses join the first controller's samples: its state and its keystrokes both see them.
  if (user == 0 && !input_script_.empty()) pad.buttons |= input_script_.ButtonsAt(at - input_script_start_);
  // The recorder keeps what the title receives, from its start for its length (a neutral state closes it).
  if (user == 0 && recording_on_ && at >= recording_start_) {
    const auto since = at - recording_start_;
    if (recording_length_ > std::chrono::steady_clock::duration::zero() && since >= recording_length_) {
      recording_.Add(recording_length_, GamepadState{});
      recording_on_ = false;
    } else {
      recording_.Add(since, pad);
    }
  }
  pads_[user].input.Sample(pad, at);
}

bool Xam::SetInputRecorder(const std::string& path, std::chrono::steady_clock::duration from,
                           std::chrono::steady_clock::duration length) {
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);  // writable, and empty until the first write
    if (!out) return false;
  }
  std::lock_guard lock(pads_mutex_);
  recording_ = InputRecording{};
  recording_path_ = path;
  recording_start_ = std::chrono::steady_clock::now() + from;
  recording_next_write_ = recording_start_;
  recording_length_ = length;
  recording_written_ = 0;
  recording_on_ = true;
  recording_open_ = true;
  return true;
}

void Xam::FlushInputRecording(bool now_anyway) {
  std::string text, path;
  bool first = false;
  {
    std::lock_guard lock(pads_mutex_);
    if (!recording_open_) return;
    const auto now = std::chrono::steady_clock::now();
    if (recording_on_ && !now_anyway && now < recording_next_write_) return;
    recording_next_write_ = now + std::chrono::milliseconds(500);
    first = recording_written_ == 0;
    text = recording_.Text(recording_written_);
    if (first && recording_.empty()) text = std::string(InputRecording::kHeader) + "\n";
    recording_written_ = recording_.events().size();
    path = recording_path_;
    if (!recording_on_) recording_open_ = false;  // its last write
  }
  std::ofstream out(path, std::ios::binary | (first ? std::ios::trunc : std::ios::app));
  out << text;
}

void Xam::SampleIfPolled(uint32_t user, std::chrono::steady_clock::time_point at) {
  if (!sampling_.load(std::memory_order_relaxed)) Sample(user, at);
}

void Xam::StartInputSampling(std::chrono::microseconds period) {
  if (sampling_.exchange(true)) return;
  sampler_ = std::thread([this, period] {
    auto next = std::chrono::steady_clock::now();
    while (sampling_.load()) {
      {
        std::lock_guard lock(pads_mutex_);
        const auto at = std::chrono::steady_clock::now();
        for (uint32_t u = 0; u < 4; ++u) {
          if (Connected(u)) Sample(u, at);
        }
      }
      FlushInputRecording();  // the recorder's file, every half second
      next += period;
      const auto now = std::chrono::steady_clock::now();
      if (next < now) next = now;
      std::this_thread::sleep_until(next);
    }
  });
}

void Xam::StopInputSampling() {
  sampling_.store(false);
  if (sampler_.joinable()) sampler_.join();
  FlushInputRecording(true);
}

Xam::~Xam() { StopInputSampling(); }

uint32_t Xam::InputGetCapabilities(Guest g, uint32_t user, uint32_t flags, uint32_t caps) {
  if (!caps) return kErrorBadArguments;
  if ((flags & 0xFF) && !(flags & kInputFlagGamepad)) return kErrorDeviceNotConnected;
  if (!Connected(ActualUser(user, flags))) return kErrorDeviceNotConnected;
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
  if (!Connected(u)) return kErrorDeviceNotConnected;
  if (!state) return kErrorSuccess;  // a query
  GamepadState pad;
  uint32_t packet;
  {
    std::lock_guard lock(pads_mutex_);
    const auto at = std::chrono::steady_clock::now();
    SampleIfPolled(u, at);
    Pad& p = pads_[u];
    pad = p.input.Present(at, input_step_);
    if (!p.seen || !(p.last == pad)) ++p.packet;
    p.seen = true;
    p.last = pad;
    packet = p.packet;
  }
  g.Store32(state, packet);
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
  if (!Connected(u)) return kErrorDeviceNotConnected;
  if (host_.vibrate) host_.vibrate(u, g.Load16(vibration), g.Load16(vibration + 2));
  return kErrorSuccess;
}

uint32_t Xam::InputGetKeystroke(Guest g, uint32_t user, uint32_t flags, uint32_t keystroke) {
  if (!keystroke) return kErrorBadArguments;
  if ((flags & 0xFF) && !(flags & kInputFlagGamepad)) return kErrorDeviceNotConnected;
  std::memset(g.host(keystroke), 0, 8);
  // XINPUT_KEYSTROKE: virtual key, unicode, flags, user, HID code.
  const bool any = (user & 0xFF) == 0xFF;
  bool connected = false;
  std::lock_guard lock(pads_mutex_);
  for (uint32_t u = any ? 0 : user; u < (any ? 4u : user + 1); ++u) {
    if (!Connected(u)) continue;
    connected = true;
    const auto at = std::chrono::steady_clock::now();
    SampleIfPolled(u, at);
    uint16_t key, key_flags;
    if (!pads_[u].input.NextKeystroke(at, key, key_flags)) continue;
    g.Store16(keystroke, key);
    g.Store16(keystroke + 4, key_flags);
    g.Store8(keystroke + 6, uint8_t(u));
    return kErrorSuccess;
  }
  return connected ? kErrorEmpty : kErrorDeviceNotConnected;
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
  const uint32_t handle = ModuleHandle(g, name);
  if (handle_ptr) g.Store32(handle_ptr, handle);
  return handle ? kStatusSuccess : kStatusNotFound;
}

uint32_t Xam::ModuleHandle(Guest g, uint32_t name) {
  const std::string module = GuestString(g, name, 64);
  std::string lower = module;
  for (char& c : lower) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
  uint32_t handle = 0;
  if (module.empty() || lower == "default.xex" || lower == title_.module_name) handle = title_.module_handle;
  else if (lower == "xam.xex" || lower == "xboxkrnl.exe") handle = SystemModule(g);
  return handle;
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

uint32_t Xam::LoadImageFromMemory(Guest g, uint32_t name, uint32_t handle_ptr) {
  constexpr uint32_t kErrorInvalidParameter = 0x57, kStatusObjectNameCollision = 0xC0000035;
  if (!handle_ptr) return kErrorInvalidParameter;
  const bool loaded = name && LoadImage(g, name, handle_ptr) == kStatusSuccess;
  g.Store32(handle_ptr, 0);
  return loaded ? kStatusObjectNameCollision : kStatusNoSuchFile;
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
  const std::filesystem::path package = ContentPath(g, content_data);
  if (package.empty() || !std::filesystem::exists(package, error)) return Finish(g, overlapped, kErrorFileNotFound);
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

uint32_t Xam::XNetStartup(Guest g, uint32_t params) {
  // XNetStartupParams: its own size first (13 bytes in the SDKs the titles use).
  if (params) {
    const uint8_t size = std::min<uint8_t>(g.Load8(params), 13);
    startup_params_.assign(g.host(params), g.host(params) + size);
  }
  return 0;
}

uint32_t Xam::XNetGetOpt(Guest g, uint32_t option, uint32_t buffer, uint32_t size_ptr) {
  constexpr uint32_t kWsaEInval = 10022, kWsaEMsgSize = 10040, kStartupParams = 1, kParamsSize = 13;
  if (option != kStartupParams || !size_ptr) return kWsaEInval;
  if (g.Load32(size_ptr) < kParamsSize) {
    g.Store32(size_ptr, kParamsSize);
    return kWsaEMsgSize;
  }
  std::memset(g.host(buffer), 0, kParamsSize);
  if (!startup_params_.empty()) std::memcpy(g.host(buffer), startup_params_.data(), startup_params_.size());
  g.Store32(size_ptr, kParamsSize);
  return 0;
}

uint32_t Xam::LogonGetMachineId(Guest g, uint32_t id_ptr) {
  if (!id_ptr) return kErrorInvalidParameter;
  g.Store64(id_ptr, title_.machine_id);
  return kErrorSuccess;
}

uint32_t Xam::WsaSetLastError(uint32_t error) {
  t_wsa_error = error;
  return 0;
}

uint32_t Xam::WsaSetEvent(Guest g, uint32_t handle) {
  const uint32_t status = Objects().NtSetEvent(g, handle, 0);
  if (status == kStatusSuccess) return 1;
  t_wsa_error = RtlNtStatusToDosError(status);
  return 0;
}

uint32_t Xam::WsaResetEvent(Guest g, uint32_t handle) {
  const uint32_t status = Objects().NtClearEvent(g, handle);
  if (status == kStatusSuccess) return 1;
  t_wsa_error = RtlNtStatusToDosError(status);
  return 0;
}

uint32_t Xam::XNetDnsLookup(Guest g, uint32_t event, uint32_t dns_ptr) {
  if (dns_ptr) {
    // XNDNS: status, address count, eight addresses.
    const uint32_t dns = AllocatePool(g, 40);
    if (dns) {
      g.Store32(dns, kWsaENetDown);
      g.Store32(dns + 4, 0);
    }
    g.Store32(dns_ptr, dns);
  }
  if (event) Objects().NtSetEvent(g, event, 0);
  return 0;
}

uint32_t Xam::XNetQosServiceLookup(Guest g, uint32_t event, uint32_t qos_ptr) {
  if (qos_ptr) {
    // XNQOS: result count, pending count, then the results (none).
    const uint32_t qos = AllocatePool(g, 0x40);
    g.Store32(qos_ptr, qos);
  }
  if (event) Objects().NtSetEvent(g, event, 0);
  return 0;
}

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
  const uint32_t event = Objects().AllocateObject(kEventSize, true);
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
