// xam.xex and the process-level kernel calls a title needs to boot and run offline: XAM messages and notifications,
// the signed-in local user and its profile settings, sessions, enumerators, content packages (saves) over the file
// system, the controllers (through an input source the host provides), the title's XEX module, the network as a
// console without a connection, and the process exits (bug checks, terminate, return to firmware) through hooks.
// The answers are those of a retail console with one local profile signed in and no Xbox Live.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "guest.h"
#include "input.h"
#include "input_bindings.h"

namespace any360::sys {

// Win32 / XAM result codes.
constexpr uint32_t kErrorSuccess = 0, kErrorNoMoreFiles = 0x12, kErrorInvalidParameter = 0x57, kErrorInsufficientBuffer = 0x7A,
                   kErrorBadArguments = 0xA0, kErrorIoPending = 0x3E5, kErrorDeviceNotConnected = 0x48F, kErrorNotFound = 0x490,
                   kErrorNoSuchUser = 0x525, kErrorEmpty = 0x10D2, kErrorAlreadyExists = 0xB7, kErrorFileNotFound = 0x2,
                   kErrorAccessDenied = 0x5, kErrorFunctionFailed = 0x65B;
// Notification ids.
constexpr uint32_t kNotifySigninChanged = 0x0000000A, kNotifySystemUi = 0x00000009;

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
  // Where the binary profile settings a title writes (XPROFILE_TITLE_SPECIFIC1..3, its options) are kept on the host,
  // one file per setting named by its id in hex holding its bytes; empty: kept in memory for the run only.
  std::filesystem::path profile_root;
  uint64_t xuid = 0xE000000000000001ull;  // the local profile (an offline XUID)
  std::string gamertag = "Player";
  uint32_t license_mask = 0;                    // XContentGetLicenseMask: no content license granted, as the reference answers
  uint64_t machine_id = 0xFA00000000000001ull;  // XNetLogonGetMachineID: the console's id
  uint32_t title_id = 0;                        // XamGetCurrentTitleId: the title's id from its XEX (0 until the host sets it)
  std::u16string title_name;                    // XamGetCachedTitleName: the title's name (empty until the host sets it)
};

class Xam {
 public:
  Xam();
  ~Xam();
  void Reset();
  XamHost& host() { return host_; }
  TitleSettings& title() { return title_; }

  // Overlapped completion as the kernel does it: result, length and extended error written, the event set. The
  // extended error is the result itself, or with `hresult` the result as an HRESULT (HRESULT_FROM_WIN32: 0x8007xxxx,
  // 0 for success), as XamEnumerate, XamContentCreate and XamUserAreUsersFriends write it: Call of Duty: Black Ops
  // takes any other extended error of an empty content enumeration for content that changed, and stops.
  void Complete(Guest g, uint32_t overlapped, uint32_t result, uint32_t length = 0, bool hresult = false);
  // A result returned directly, or through the overlapped structure (then ERROR_IO_PENDING).
  uint32_t Finish(Guest g, uint32_t overlapped, uint32_t result, uint32_t length = 0, bool hresult = false);
  void Notify(uint32_t id, uint32_t param);

  // Messages and notifications.
  uint32_t MsgStartIORequest(Guest g, uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer, uint32_t length);
  uint32_t MsgInProcessCall(uint32_t app, uint32_t message, uint32_t arg1, uint32_t arg2);
  uint32_t MsgCancelIORequest(Guest g, uint32_t overlapped, uint32_t wait);
  // XMsgSystemProcessCall: a message to a system application, answered at once as the in-process form.
  uint32_t MsgSystemProcessCall(uint32_t app, uint32_t message, uint32_t buffer, uint32_t length);
  // XMsgCompleteIORequest: a request completed by the title itself (result, extended error and length).
  uint32_t MsgCompleteIORequest(Guest g, uint32_t overlapped, uint32_t result, uint32_t extended_error, uint32_t length);
  // XGetOverlappedResult: the result of a completed request (its length stored); a pending one is ERROR_IO_INCOMPLETE
  // unless `wait` asks to wait for its event.
  uint32_t GetOverlappedResult(Guest g, uint32_t overlapped, uint32_t length_ptr, uint32_t wait, uint32_t thread);
  uint32_t NotifyCreateListener(Guest g, uint64_t mask, uint32_t max_version);
  uint32_t NotifyGetNext(Guest g, uint32_t handle, uint32_t match_id, uint32_t id_ptr, uint32_t param_ptr);

  // The local user and profile.
  uint32_t UserGetSigninState(uint32_t user) const;
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
  // XamShowSigninUIEx (users, flags, overlapped): XamShowSigninUI's answer; through an overlapped, Xenia's immediate
  // completion (the result as result and extended error, the length ~0 on failure) and ERROR_IO_PENDING.
  uint32_t ShowSigninUIEx(Guest g, uint32_t users, uint32_t flags, uint32_t overlapped);
  // XamShowMarketplaceUIEx (user, entry point, offer id, offer type, category, two words, title id): the marketplace
  // needs Xbox Live, so the guide opens and closes at once (as XamShowMarketplaceUI); ERROR_INVALID_PARAMETER for a
  // user past 3 or an entry point past the last (24), ERROR_NO_SUCH_USER for a user not signed in, as Xenia.
  uint32_t ShowMarketplaceUIEx(uint32_t user, uint32_t entry_point);
  // XamUserGetGamerTag: the local profile's gamertag, truncated to `length` characters (16 at most) with a terminator;
  // E_INVALIDARG for a user past 3, no buffer or under 16 characters, the NO_SUCH_USER HRESULT for users 1 to 3.
  uint32_t UserGetGamerTag(Guest g, uint32_t user, uint32_t buffer, uint32_t length);
  // Parental controls: none set; every content access allowed (1), completed through the overlapped when given.
  uint32_t UserContentRestrictionGetFlags(Guest g, uint32_t user, uint32_t flags_out);
  uint32_t UserContentRestrictionGetRating(Guest g, uint32_t user, uint32_t rating_out, uint32_t extra_out);
  uint32_t UserContentRestrictionCheckAccess(Guest g, uint32_t allowed_out, uint32_t overlapped);
  // XamUserGetDeviceContext: no device context (0); success for user 0 or "any user" (0xFF in the low byte), else the
  // DEVICE_NOT_CONNECTED HRESULT, as the reference.
  uint32_t UserGetDeviceContext(Guest g, uint32_t user, uint32_t context_out);
  // XamGetOnlineSchema: the address of a (schema pointer, size) pair describing an empty Live schema (the reference's
  // 44 bytes), created once in the kernel heap.
  uint32_t GetOnlineSchema(Guest g);
  uint32_t ShowMessageBoxUI(Guest g, uint32_t result_ptr, uint32_t overlapped);
  uint32_t ShowDeviceSelectorUI(Guest g, uint32_t device_id_ptr, uint32_t overlapped);
  // The virtual keyboard, accepted at once with its default text (the buffer emptied without one), truncated to the
  // buffer's `length` characters, terminator included.
  uint32_t ShowKeyboardUI(Guest g, uint32_t default_text, uint32_t buffer, uint32_t length, uint32_t overlapped);
  // Guide screens that need Xbox Live (friends, gamer cards, marketplace): the guide opens and closes at once.
  uint32_t ShowLiveUI();
  // XamShowMarketplaceDownloadItemsUI (user, entry point, offer ids, offer count, 0, HRESULT out, overlapped, 0): the
  // marketplace's download screen for the given offers, which needs Xbox Live: the guide opens and closes at once,
  // S_OK in the HRESULT, the request completed (a title waits for its overlapped). A user index above 3 is refused.
  uint32_t ShowMarketplaceDownloadItemsUI(Guest g, uint32_t user, uint32_t result_ptr, uint32_t overlapped);
  // The local profile has no Live friends; other users are not signed in.
  uint32_t UserAreUsersFriends(Guest g, uint32_t user, uint32_t out, uint32_t overlapped);
  // An enumerator of the title's achievements as the local profile holds them: none known (the enumeration ends at
  // once). The buffer size per call is written as the reference does.
  uint32_t UserCreateAchievementEnumerator(Guest g, uint32_t user, uint32_t flags, uint32_t count, uint32_t size_ptr,
                                           uint32_t handle_ptr);
  // Gamer pictures: the local profile's tile key parses to placeholder ids; a tile reads as a plain image.
  uint32_t ParseGamerTileKey(Guest g, uint32_t key, uint32_t out1, uint32_t out2, uint32_t out3);
  uint32_t ReadTileToTexture(Guest g, uint64_t tile_id, uint32_t buffer, uint32_t stride, uint32_t height, uint32_t overlapped);
  // Step 12, the information calls with XAM state (Xenia's xam_info.cc). The active dashboard application's 12-byte
  // info: the last one set (zero at first, cleared by a null set); a null get writes nothing.
  void GetActiveDashAppInfo(Guest g, uint32_t info) const;
  void SetActiveDashAppInfo(Guest g, uint32_t info);
  // XamGetDashContext: the dashboard context (0: none set). XamSetDashContext stores another one and notifies
  // XN_SYS_DASHCONTEXTCHANGED (0x8000000C) when it changes; 0.
  uint32_t DashContext() const { return dash_context_; }
  uint32_t SetDashContext(uint32_t context);

  // Step 12, the guide screens and Kinect (NUI) calls Xenia implements (its xam_ui.cc, xam_nui.cc). The guide opens
  // and closes at once, as ShowLiveUI. Profiles: one local profile (user 0) and no way to make another, so
  // XamShowCreateProfileUI(Ex) opens and closes the guide; the edit-profile and gamer-card screens refuse a user
  // that is not signed in (ERROR_ACCESS_DENIED); the forced name change (a marketplace screen) refuses a user past 3
  // (ERROR_INVALID_PARAMETER) or not signed in (ERROR_NO_SUCH_USER); the multiplayer (Gold) upgrade needs Xbox Live
  // and fails (ERROR_FUNCTION_FAILED); the Kinect-required screen takes only 0 (else ERROR_INVALID_PARAMETER) and
  // closes at once, as the troubleshooter.
  uint32_t ShowCreateProfileUI();
  uint32_t ShowProfileUI(uint32_t user);
  uint32_t ShowForcedNameChangeUI(uint32_t user);
  uint32_t ShowMultiplayerUpgradeUI() const { return kErrorFunctionFailed; }
  uint32_t ShowNuiHardwareRequiredUI(uint32_t argument) const { return argument ? kErrorInvalidParameter : kErrorSuccess; }
  // No Kinect: not ready, the HUD not enabled, no initialize flags, versions 0, no engaged player (setting one
  // fails with E_FAIL, clearing it succeeds), the chat microphone off. The identity session id is a random non-zero
  // 64-bit value made at the first call, as the console's (Xenia uses a constant).
  uint32_t NuiHudSetEngagedTrackingId(uint32_t id) const { return id ? 0x80004005u : 0; }
  void NuiHudGetVersions(Guest g, uint32_t xam_version, uint32_t xtl_version) const;
  uint64_t NuiIdentitySessionId();

  // Step 12, the profile and user calls Xenia implements (its xam_profile.cc, xam_user.cc, xam_enum.cc, xam_nui.cc,
  // user_tracker.cc). One local profile (TitleSettings: xuid, gamertag), signed in as user 0. Its account
  // (X_XAMACCOUNTINFO, 0x17C bytes): the gamertag, no online XUID, no reserved flags, and as cached user flags the
  // console's country (bits 8-15) and language (bits 25-29), no subscription tier, no parental controls (Xenia leaves
  // the country and language at 0 and reads the console's instead).
  static constexpr uint32_t kAccountSize = 0x17C, kProfileEnumResultSize = 0x188;
  void WriteAccount(Guest g, uint32_t account) const;
  void WriteProfileEnumResult(Guest g, uint32_t at) const;
  uint32_t CachedUserFlags() const;
  // XamUserGetUserFlags / XamUserGetCachedUserFlags (user index) and XamUserGetUserFlagsFromXUID: the flags above for
  // the local profile, 0 otherwise.
  uint32_t UserFlags(uint32_t user) const { return UserGetSigninState(user) ? CachedUserFlags() : 0; }
  uint32_t UserFlagsFromXuid(uint64_t xuid) const { return xuid == title_.xuid ? CachedUserFlags() : 0; }
  // XamProfileCreateEnumerator: an enumerator of the profiles on the console (the local one: X_PROFILEENUMRESULT,
  // its offline XUID, account and device 1), one a call; ERROR_INVALID_PARAMETER without a handle out.
  uint32_t ProfileCreateEnumerator(Guest g, uint32_t handle_ptr);
  // XamProfileEnumerate (handle, flags, buffer, overlapped): the next profile into the buffer, which the system
  // gives no size for; ERROR_NO_MORE_FILES at the end.
  uint32_t ProfileEnumerate(Guest g, uint32_t handle, uint32_t buffer, uint32_t overlapped);
  // XamProfileFindAccount (offline XUID, account out, device out): the account cleared, then the local profile's
  // and device 1; ERROR_NO_SUCH_USER for another XUID, ERROR_INVALID_PARAMETER without an account out.
  uint32_t ProfileFindAccount(Guest g, uint64_t xuid, uint32_t account, uint32_t device_ptr) const;
  // XamProfileOpen (XUID, mount name, flags, XCONTENT_DATA_AGGREGATE out): the local profile's package mounted as
  // "<name>:" ("User_<XUID>" without a name; a folder under the content root), the content data filled (profile
  // type 0x10000, device 1, the XUID as file name, the dashboard's title id); ERROR_INVALID_PARAMETER for another XUID.
  uint32_t ProfileOpen(Guest g, uint64_t xuid, uint32_t mount_name, uint32_t content_data);
  // XamUserGetIndexFromXUID: 0 for the local profile's XUID; E_INVALIDARG without an index out, the NO_SUCH_USER
  // HRESULT for another XUID.
  uint32_t UserGetIndexFromXuid(Guest g, uint64_t xuid, uint32_t index_ptr) const;
  // XamUserGetSubscriptionType (user, length out, payment out): no Xbox Live subscription (0, 0) for user 0;
  // E_INVALIDARG for a user past 3 or a null out, the NO_SUCH_USER HRESULT for another user.
  uint32_t UserGetSubscriptionType(Guest g, uint32_t user, uint32_t length_ptr, uint32_t payment_ptr) const;
  // XamUserGetUserTenure: the profile holds no tenure settings, so nothing is written; success for user 0, the
  // NO_SUCH_USER HRESULT for another.
  uint32_t UserGetUserTenure(uint32_t user) const { return UserGetSigninState(user) ? 0 : 0x80070525u; }
  // XamUserLogon (XUIDs, flags, overlapped): ERROR_INVALID_PARAMETER without XUIDs; else success (through the
  // overlapped when given). The one local profile stays signed in: adding it changes nothing, and Any360 has no other
  // profile to add nor a sign-out state (removing or logging off every user leaves it signed in).
  uint32_t UserLogon(Guest g, uint32_t xuids, uint32_t overlapped);
  // The profile and user calls Xenia stubs too (its xam_profile.cc, xam_user.cc, xam_nui.cc).
  // XamProfileClose (mount name): a profile mounted by XamProfileOpen (or content opened under that name) unmounted;
  // ERROR_FUNCTION_FAILED for a name nothing is mounted under.
  uint32_t ProfileClose(Guest g, uint32_t mount_name);
  // XamProfileCreate (flags, device out, XUID, account, payment, tokens, creation info out): Any360 keeps one local
  // profile and makes no other (as XamShowCreateProfileUI), so the creation fails as Xenia's does when its profile
  // manager cannot create one: ERROR_INVALID_PARAMETER, the device out set to 1 (as Xenia's). Unlike Xenia, which
  // allocates the creation info first (and keeps it), no info is handed out: 0.
  uint32_t ProfileCreate(Guest g, uint32_t device_ptr, uint32_t creation_info_ptr) const;
  // XamProfileGetCreationStatus (X_PROFILE_CREATION_INFO, XUID out): success; with an info, its offline XUID (at
  // 0xA78) copied out. Any360 hands out no info, so none is freed.
  uint32_t ProfileGetCreationStatus(Guest g, uint32_t creation_info, uint32_t xuid_ptr) const;
  // XamProfileLoadAccountInfo (device, XUID, account out): the local profile's account (X_XAMACCOUNTINFO);
  // ERROR_INVALID_PARAMETER without an account out, for XUID 0 or another XUID.
  uint32_t ProfileLoadAccountInfo(Guest g, uint64_t xuid, uint32_t account) const;
  // XamUserCreateTitlesPlayedEnumerator (title id, user, XUID, first, count, size out, handle out): the titles the
  // profile played that have achievements (Xenia's user tracker lists only those); Any360's profile has none, so an
  // empty enumerator of XTITLE_PLAYED items (0xA8 bytes: X_XDBF_GPD_TITLE_PLAYED and a 64-character name), its buffer
  // size count * 0xA8. ERROR_INVALID_PARAMETER for a user past 3, no count, size or handle out;
  // ERROR_FUNCTION_FAILED for a user not signed in.
  uint32_t UserCreateTitlesPlayedEnumerator(Guest g, uint32_t user, uint32_t count, uint32_t size_ptr, uint32_t handle_ptr);
  // XamUserIsUnsafeProgrammingAllowed (user, check, result out): allowed (1); ERROR_INVALID_PARAMETER without a result
  // out or for a user past 3 that is not XUSER_INDEX_ANY (0xFF).
  uint32_t UserIsUnsafeProgrammingAllowed(Guest g, uint32_t user, uint32_t result_ptr) const;
  // XamUserNuiGetUserIndexForSignin: user 0 (the signed-in profile), S_OK; E_INVALIDARG without an index out.
  uint32_t UserNuiGetUserIndexForSignin(Guest g, uint32_t index_ptr) const;
  // XamUserReadUserPreference / XamUserWriteUserPreference (user, mask, value [out], overlapped): the dashboard's
  // XPROFILE_USER_PREFERENCES word, kept for the run. A mask names one preference (0x001, 0x020 ... 0x200 alone; 0x006
  // and 0x018 with a "written" bit, 0x004 and 0x010); a read gives the word's bits under the mask, without the written
  // bit (S_FALSE as the extended error when never written); a write takes only bits of the mask, never the written
  // bit, sets it, and notifies XN_SYS_PROFILESETTINGCHANGED (0x0E) for user 0. ERROR_INVALID_PARAMETER for a user past
  // 3 (or no value out); ERROR_FUNCTION_FAILED with E_INVALIDARG or NO_SUCH_USER as the extended error otherwise.
  uint32_t UserReadUserPreference(Guest g, uint32_t user, uint32_t mask, uint32_t value_ptr, uint32_t overlapped);
  uint32_t UserWriteUserPreference(Guest g, uint32_t user, uint32_t mask, uint32_t value, uint32_t overlapped);
  // XamGetDashBackstackData: no dashboard ran before the title, so no backstack nodes: nothing written, success.
  uint32_t GetDashBackstackData() const { return 0; }
  // XamGetCachedTitleName: the title's name for any id but 0 (one profile, one title known), as big-endian UTF-16
  // with its terminator, its length with the terminator stored; for id 0 an empty string, length 1 and
  // ERROR_INVALID_PARAMETER. The whole name is written, as Xenia does: the length is not read first.
  uint32_t GetCachedTitleName(Guest g, uint32_t title_id, uint32_t buffer, uint32_t length_ptr) const;
  // XamGetLanguageLocaleFallbackString: the guest address of the language's fallback locale name (info.h:
  // LanguageLocaleFallback), the 18 laid out once in the kernel heap; 0 for a language past Simplified Chinese.
  uint32_t LanguageLocaleFallbackString(Guest g, uint32_t language);
  // XamGetLanguageTypefacePatch: the guest address of the font patch a language would use, as Xenia gives it (big-
  // endian UTF-16 "file://media:/Xenon<SC|C|JK>Latin.xttp2": Simplified Chinese (17), Traditional Chinese (8), any
  // other), the three laid out once in the kernel heap; 0 when the heap is full.
  uint32_t LanguageTypefacePatch(Guest g, uint32_t language);
  // XamGetLiveHiveValueA / W (feature name, value out, its size, flag, overlapped): no Xbox Live to sign in to:
  // X_ONLINE_E_LOGON_NOT_LOGGED_ON (0x80151802), E_INVALIDARG without a name, a value buffer or its size; through an
  // overlapped, that result as both result and extended error, the length ~0 (Xenia's immediate completion).
  uint32_t LiveHiveValue(Guest g, uint32_t name, uint32_t value, uint32_t size, uint32_t overlapped);
  // XamQueryLiveHiveA (feature name, value out, its size, overlapped): no Live hive offline. Without an overlapped the
  // extended error is the answer (X_ONLINE_E_LOGON_NOT_LOGGED_ON, or E_INVALIDARG without a name, a value or its
  // size); with one, it completes with success and that extended error, and E_PENDING (0x8000000A) is returned, as
  // Xenia.
  uint32_t QueryLiveHiveA(Guest g, uint32_t name, uint32_t value, uint32_t size, uint32_t overlapped);
  // XamIptvGetServiceName: the guest address of the IPTV provider's name (0x78 bytes, 59 UTF-16 characters and a
  // terminator, laid out once in the kernel heap) written at `name_out`; 0, or E_OUTOFMEMORY when the heap has no
  // room. The name is the console's IPTV setting, which is empty here (IPTV is off, as XamIsIptvEnabled answers);
  // Xenia writes its own name when the setting is empty.
  uint32_t IptvServiceName(Guest g, uint32_t name_out);

  // Step 12, ninth batch (Xenia's xam_content.cc, xam_info.cc, xam_net.cc, xam_user.cc).
  // XamContentCreateEnumeratorInternal (XUID, device, type, title id, flags, items a call, buffer size out, handle out):
  // the packages of a type as XCONTENT_DATA items, as XamContentCreateEnumerator lists them (one local profile and one
  // title: the XUID and title id select nothing more). A device other than 0 (any), 1 (the hard drive) or 2 (the disc,
  // which holds no package here), or no handle pointer: the size written as 0 and E_INVALIDARG. The size is
  // 0x134 times the items a call.
  uint32_t ContentCreateEnumeratorInternal(Guest g, uint32_t device_id, uint32_t content_type, uint32_t per_call,
                                           uint32_t size_ptr, uint32_t handle_out);
  // XamContentGetMetaDataInternal (XCONTENT_DATA_INTERNAL, XCONTENT_METADATA out, overlapped): the package's 0x93D6-byte
  // metadata as Xenia's folder packages give it, from the header the package was created with (an
  // XCONTENT_AGGREGATE_DATA): its type, version 2, one data file, an STFS volume, the header's title id in the execution
  // info and its display name in the English slot, the rest 0; all 0 for a package without a header. The content
  // data only names the package. ERROR_FUNCTION_FAILED when the package does not exist, nothing written and the
  // overlapped left alone (as Xenia); otherwise completed through the overlapped when given.
  uint32_t ContentGetMetaDataInternal(Guest g, uint32_t content_data, uint32_t metadata, uint32_t overlapped);
  // XamContentRegisterChangeCallback / XamLoaderRegisterLaunchRequestCallback: the title's callback kept (no content
  // changes and no launch requests come from outside a title here, so neither is called).
  void ContentRegisterChangeCallback(uint32_t callback) { content_change_callback_ = callback; }
  uint32_t content_change_callback() const { return content_change_callback_; }
  // XamNuiCameraTiltSetCallback: the title's tilt callback kept (no Kinect motor ever calls it).
  void NuiCameraTiltSetCallback(uint32_t callback) { nui_tilt_callback_ = callback; }
  uint32_t nui_tilt_callback() const { return nui_tilt_callback_; }
  void LoaderRegisterLaunchRequestCallback(uint32_t callback) { launch_request_callback_ = callback; }
  uint32_t launch_request_callback() const { return launch_request_callback_; }
  // XamReadString (title id, string id, user, buffer, size in/out, overlapped): the title's localized string, from its
  // SPA's string table in Xenia's game database; Any360 keeps no such table, so the string is empty: the terminator
  // written when the size is not 0. E_INVALIDARG for no buffer or id 0xFFFF, ERROR_INSUFFICIENT_BUFFER (an HRESULT)
  // for no size pointer; completed through the overlapped when given.
  uint32_t ReadString(Guest g, uint64_t id, uint32_t buffer, uint32_t size_ptr, uint32_t overlapped);
  // XamNetworkStorageUserHasPrivilege (user index): whether the user (or, for 0xFF, any user) holds the network
  // storage privilege (0xD1). Xenia grants it only to a profile signed in to Xbox Live; the local profile is offline,
  // so 0 for every user.
  uint32_t NetworkStorageUserHasPrivilege(uint32_t) const { return 0; }
  // XamNetworkStorageHasUserEnabledStorage (user index): 1 when the user is signed in and its network storage
  // preference (XPROFILE_USER_PREFERENCES bit 0x20, as XamUserWriteUserPreference sets it) is on; otherwise the
  // privilege decides, which no user holds offline: 0. As Xenia.
  uint32_t NetworkStorageHasUserEnabledStorage(uint32_t user) const;
  // The title id of the XEX header's execution info (0 without a header).
  uint32_t TitleId(Guest g) const;
  // Tasks: the callback runs on a new thread with the message as its context.
  uint32_t TaskSchedule(Guest g, uint32_t callback, uint32_t message, uint32_t handle_ptr);
  // Voice: no headset or voice device.
  uint32_t VoiceCreate(Guest g, uint32_t voice_ptr);

  // Enumerators: a handle on items of `item_size` bytes, handed out `per_call` at a time.
  uint32_t CreateEnumerator(Guest g, uint32_t item_size, uint32_t per_call, std::vector<std::vector<uint8_t>> items);
  uint32_t Enumerate(Guest g, uint32_t handle, uint32_t flags, uint32_t buffer, uint32_t length, uint32_t items_ptr, uint32_t overlapped);
  uint32_t GetPrivateEnumStructure(Guest g, uint32_t handle, uint32_t out);

  // Content packages: folders under content_root/<type>/<file name>, mounted as "<root name>:" while open.
  // Each keeps the XCONTENT_DATA it was created with (its display name) in content_root/Headers/<type>/<file name>.header.
  uint32_t ContentCreate(Guest g, uint32_t user, uint32_t root_name, uint32_t content_data, uint32_t flags, uint32_t disposition_ptr,
                         uint32_t license_ptr, uint32_t overlapped);
  uint32_t ContentClose(Guest g, uint32_t root_name, uint32_t overlapped);
  uint32_t ContentDelete(Guest g, uint32_t user, uint32_t content_data, uint32_t overlapped);
  uint32_t ContentCreateEnumerator(Guest g, uint32_t user, uint32_t device_id, uint32_t content_type, uint32_t flags, uint32_t per_call,
                                   uint32_t size_ptr, uint32_t handle_out);
  uint32_t ContentGetDeviceState(Guest g, uint32_t device_id, uint32_t overlapped);
  // XamContentAggregateCreateEnumerator: the packages of a type on the hard drive (device 0 or 1) as
  // XCONTENT_AGGREGATE_DATA, one per call, with the local profile's XUID and the title's id; E_INVALIDARG otherwise.
  uint32_t ContentAggregateCreateEnumerator(Guest g, uint32_t device_id, uint32_t content_type, uint32_t handle_out);
  // The package's thumbnail (PNG bytes), kept next to its folder as "<file name>.png"; ERROR_FILE_NOT_FOUND when the
  // package does not exist.
  uint32_t ContentSetThumbnail(Guest g, uint32_t content_data, uint32_t buffer, uint32_t size, uint32_t overlapped);
  uint32_t ContentGetLicenseMask(Guest g, uint32_t mask_ptr, uint32_t overlapped);
  // The thumbnail ContentSetThumbnail kept: its size written, the bytes copied when a buffer is given (and big enough).
  uint32_t ContentGetThumbnail(Guest g, uint32_t content_data, uint32_t buffer, uint32_t size_ptr, uint32_t overlapped);
  // XamContentResolve: the device path a package's files are reached by ("\Device\Content\<type>\<file name>",
  // mounted on its folder), for RtlInitAnsiString.
  uint32_t ContentResolve(Guest g, uint32_t content_data, uint32_t buffer, uint32_t size);
  // The devices content can be stored on (the hard drive only) as an enumerator of XDEVICE_DATA, and a device's name.
  uint32_t ContentCreateDeviceEnumerator(Guest g, uint32_t per_call, uint32_t size_ptr, uint32_t handle_out);
  uint32_t ContentGetDeviceName(Guest g, uint32_t device_id, uint32_t buffer, uint32_t capacity);

  // The content calls Xenia stubs too (its xam_content.cc, xam_content_device.cc, xam_user.cc). The internal forms
  // take an XCONTENT_DATA_INTERNAL, whose start is an XCONTENT_DATA (device, type, display name, file name at 0x108).
  // XamContentGetAttributesInternal (content data, X_FILE_ATTRIBUTE_DATA out, overlapped): the package folder's
  // attributes (0, as Xenia), creation, last access and last write times, size 0 (a folder); ERROR_FUNCTION_FAILED,
  // the overlapped untouched, when the package does not exist.
  uint32_t ContentGetAttributesInternal(Guest g, uint32_t content_data, uint32_t attributes, uint32_t overlapped);
  // XamContentGetDeviceVolumePath (device, path out, size, append backslash): the hard drive's volume, "hdd0\"
  // (truncated to the size, terminated); ERROR_FUNCTION_FAILED for another device.
  uint32_t ContentGetDeviceVolumePath(Guest g, uint32_t device_id, uint32_t path, uint32_t size);
  // XamContentGetLocalizedDeviceData (device, X_CONTENT_DEVICE_DATA out): XamContentGetDeviceData's, named by the
  // device's type ("Hard Drive", "Optical Drive"); ERROR_DEVICE_NOT_CONNECTED for another device.
  uint32_t ContentGetLocalizedDeviceData(Guest g, uint32_t device_id, uint32_t data);
  // XamContentGetLocalizedString (XCONTENT_METADATA, name out, its size, description out, its size): the display name
  // and description in the console's language (languages 10-12 only in metadata of version 2 or later, else empty;
  // past 12 English), truncated to each size in characters and terminated; ERROR_INVALID_PARAMETER without metadata.
  uint32_t ContentGetLocalizedString(Guest g, uint32_t metadata, uint32_t name, uint32_t name_size, uint32_t description,
                                     uint32_t description_size) const;
  // XamContentResolveInternal (content data, path out, size, create directory, root name, overlapped): the device
  // path of a package on the hard drive ("\Device\Content\<type>\<file name>", mounted on its folder, which is made
  // when asked), or through a root name ("<name>:", linked to it; ERROR_ALREADY_EXISTS, the path still given, when the
  // name is taken); on the disc Xenia's "D:\content\<XUID for saved games>\<title id>\<type>\<file name>" (no
  // package is there). Truncated to the size and terminated. ERROR_INVALID_PARAMETER for another device, a root name
  // off the hard drive or a package name that names no folder.
  uint32_t ContentResolveInternal(Guest g, uint32_t content_data, uint32_t path, uint32_t size, uint32_t create_directory,
                                  uint32_t root_name, uint32_t overlapped);
  // XamReadTile / XamReadTileEx (tile type, title id, item id, user, buffer, size in/out, overlapped): the profile has
  // no tiles (gamer pictures, achievement images), so the size 0 and ERROR_FILE_NOT_FOUND, or
  // ERROR_INSUFFICIENT_BUFFER without a buffer (the extended error its HRESULT). ERROR_INVALID_PARAMETER when neither
  // the user index nor the item id (as a XUID) names the local profile, or without a size.
  uint32_t ReadTile(Guest g, uint32_t user, uint64_t item_id, uint32_t buffer, uint32_t size_ptr, uint32_t overlapped);

  // Controllers, each read through an InputQueue (input.h): no change is lost however fast or slow the title polls.
  // The state's packet number moves on when the presented state changes (and at the first read); keystrokes are the
  // console's: made from the sampled transitions (buttons, triggers past 30, sticks past half way in eight
  // directions), a held one repeating after 400 ms every 100 ms; user 0xFF takes the first user with one.
  // The controllers sampled on a thread of their own every `period` until StopInputSampling (without it, each poll
  // samples), and how far apart a poll's steps through the queued changes are (one console frame; 0: every poll).
  void StartInputSampling(std::chrono::microseconds period = std::chrono::microseconds(1000));
  void StopInputSampling();
  void SetInputStep(std::chrono::steady_clock::duration step) { input_step_ = step; }
  // Button presses added to the first controller's samples (InputScript: its state and keystrokes), timed from this
  // call.
  void SetInputScript(InputScript script) {
    std::lock_guard lock(pads_mutex_);
    input_script_ = std::move(script);
    input_script_start_ = std::chrono::steady_clock::now();
  }
  // The launcher's bindings (input_bindings.h), layer 1: every controller's samples mapped with the "gamepad" profile
  // before the queue sees them (the input script's presses are the title's buttons, added after). An empty
  // InputBindings is the identity.
  void SetInputBindings(const InputBindings& bindings) {
    std::lock_guard lock(pads_mutex_);
    for (Pad& p : pads_) p.mapper = InputMapper(bindings, "gamepad");
  }
  uint32_t InputGetCapabilities(Guest g, uint32_t user, uint32_t flags, uint32_t caps);
  uint32_t InputGetState(Guest g, uint32_t user, uint32_t flags, uint32_t state);
  uint32_t InputSetState(Guest g, uint32_t user, uint32_t vibration);
  uint32_t InputGetKeystroke(Guest g, uint32_t user, uint32_t flags, uint32_t keystroke);

  // The title module.
  uint32_t ImageXexHeaderField(Guest g, uint32_t header, uint32_t key) const;
  uint32_t GetModuleHandle(Guest g, uint32_t name, uint32_t handle_ptr);
  // The module handle GetModuleHandle gives (0: not found).
  uint32_t ModuleHandle(Guest g, uint32_t name);
  // XexLoadImage / XexUnloadImage: the title's own module and the system ones ("xam.xex", "xboxkrnl.exe", already
  // loaded) by name, a path's directories ignored; no other image is loaded (STATUS_NO_SUCH_FILE).
  uint32_t LoadImage(Guest g, uint32_t name, uint32_t handle_ptr);
  // XexLoadImageFromMemory (image, size, name, flags, minimum version, handle out), as Xenia's xeXexLoadImage: no handle
  // pointer is ERROR_INVALID_PARAMETER (a Win32 code, as Xenia returns it); a name of a module already loaded (the
  // title's, xam.xex, xboxkrnl.exe) writes 0 and is STATUS_OBJECT_NAME_COLLISION. Any other image would be new code,
  // which a statically recompiled title cannot run: it is not loaded (0, STATUS_NO_SUCH_FILE, Xenia's answer when an
  // image does not load).
  uint32_t LoadImageFromMemory(Guest g, uint32_t name, uint32_t handle_ptr);

  // Step 12, the calls Xenia stubs too: avatars (Xenia's xam_avatar.cc). Avatars are not initialized
  // (XamAvatarInitialize fails), so a title meets these only past that failure.
  // The asynchronous ones that do nothing (asset icon, binary, enumeration begin and end, mipmaps, animation, the
  // metadata of random and signed-out avatars, manifests by XUID, wear-now): success, completed through the
  // overlapped when given (ERROR_IO_PENDING). XamAvatarWearNow run by the avatar editor (0x584D07D1) completes nothing.
  uint32_t AvatarComplete(Guest g, uint32_t overlapped) { return Finish(g, overlapped, kErrorSuccess); }
  uint32_t AvatarWearNow(Guest g, uint32_t overlapped);
  // XamAvatarGetAssets: no asset; through an overlapped ERROR_FUNCTION_FAILED with E_FAIL as its extended error, as
  // Xenia completes it; 0 without one.
  uint32_t AvatarGetAssets(Guest g, uint32_t overlapped);
  // XamAvatarGetAssetsResultSize (mask, result size out, GPU resource size out): both 0.
  uint32_t AvatarGetAssetsResultSize(Guest g, uint32_t result_size_ptr, uint32_t gpu_size_ptr);
  // XamAvatarGetInstalledAssetPackageDescription (X_ASSET_ID, XCONTENT_DATA_AGGREGATE out): the package an asset
  // installs to: avatar item (0x9000), XUID 0, the asset's title id, the file name "%016X%08X%08X" of the asset's
  // id, second id and title id. Xenia reads the X_ASSET_ID's fields without swapping them; they are read big-endian,
  // as a title writes them, here.
  void AvatarGetInstalledAssetPackageDescription(Guest g, uint32_t asset_id, uint32_t content_data);
  // XamAvatarGetInstrumentation (unknown, value out): the value 0, the result 1.
  uint32_t AvatarGetInstrumentation(Guest g, uint32_t value_ptr);
  // XamAvatarGetManifestLocalUser (user, X_AVATAR_METADATA out, overlapped): the avatar XamAvatarSetManifest kept in
  // the profile (setting XPROFILE_GAMERCARD_AVATAR_INFO_1, 0x63E80044, as the dashboard keeps it). A user past 3 or no
  // buffer: ERROR_INVALID_PARAMETER with E_INVALIDARG; a user with no profile (not 0): ERROR_FUNCTION_FAILED with
  // E_NO_SUCH_USER; no avatar kept: ERROR_FUNCTION_FAILED with E_FAIL. Without an overlapped the extended error
  // (an HRESULT) is returned on failure, as Xenia does.
  uint32_t AvatarGetManifestLocalUser(Guest g, uint32_t user, uint32_t metadata, uint32_t overlapped);
  // XamAvatarSetManifest (user, avatar info, overlapped): the avatar's 1000 bytes kept in the profile as
  // XPROFILE_GAMERCARD_AVATAR_INFO_1 (written to profile_root as XamUserWriteProfileSettings keeps a binary
  // setting); a user with no profile: ERROR_FUNCTION_FAILED with E_NO_SUCH_USER.
  uint32_t AvatarSetManifest(Guest g, uint32_t user, uint32_t info, uint32_t overlapped);

  // Media and the dashboard's services (Xenia's xam_media.cc, xam_enum.cc, xam_net.cc, xam_info.cc), stubs as Xenia's.
  // XamMediaVerificationFailedBlocks (X_SECURITY_FAILURE_INFORMATION, 0x18 bytes, its size first): no read or hash
  // failed, no block checked, the check complete (1); a size other than 0x18 is ERROR_NOT_ENOUGH_MEMORY (8). No
  // structure: success.
  uint32_t MediaVerificationFailedBlocks(Guest g, uint32_t information);
  // XampXAuthStartup (XAUTH_SETTINGS): the service is not there: 0x80158401 for settings whose size field is not 8 (or
  // none), else 0x80158406, as Xenia answers.
  uint32_t XAuthStartup(Guest g, uint32_t settings);
  uint32_t UnloadImage(uint32_t handle);
  // XexLoadImageHeaders: the XEX2 headers of the image an ANSI_STRING path names, copied to `buffer` (its first
  // header-size bytes); STATUS_NO_SUCH_FILE, STATUS_INVALID_IMAGE_FORMAT, STATUS_BUFFER_TOO_SMALL otherwise.
  uint32_t LoadImageHeaders(Guest g, uint32_t path, uint32_t buffer, uint32_t size);
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

  // The system management controller (HalSendSMCMessage, HalOpenCloseODDTray), as Xenia's model answers. A message
  // and its response are 16 bytes: the command, then its data. Queries answered (the response's command byte and its
  // fields written, the rest left): temperatures (0x07: CPU, GPU, EDRAM 69.6 C, motherboard 69.9 C, each as Xenia's two
  // bytes), the tray (0x0A), the AV pack (0x0F: VGA, 0x5B, the layer's XGetAVPack), the SMC version (0x12: 65, 2, 1),
  // the IR address (0x16: 0x0F), the tilt (0x17: vertical, 0); set the tray (0x8B, (data & 15) % 5), the CPU and GPU
  // fan speeds (0x89, 0x94), the IR address (0x95, answered with the current one, as Xenia), the power LED (0x8C),
  // the LEDs (0x99). Any other command: nothing. The tray starts closed (a title runs from its disc; Xenia starts it
  // open); a change notifies XN_SYS_TRAYSTATECHANGED (0x8000000D) with the new state.
  void SmcMessage(Guest g, uint32_t message, uint32_t response);
  void SmcSetTray(uint8_t state);
  uint8_t smc_tray() const { return smc_tray_; }
  uint32_t smc_fan_speed(bool gpu) const { return gpu ? smc_gpu_fan_ : smc_cpu_fan_; }
  uint16_t smc_power_led() const { return smc_power_led_; }
  uint16_t smc_leds() const { return smc_leds_; }

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
  // Winsock events: manual-reset kernel events behind handles. The wait takes milliseconds (0xFFFFFFFF: for ever)
  // and answers WSA_WAIT_EVENT_0 + index (wait-any), 0 (wait-all), WSA_WAIT_TIMEOUT or WSA_WAIT_FAILED.
  uint32_t WsaCreateEvent();
  uint32_t WsaCloseEvent(uint32_t handle);
  uint32_t WsaWaitForMultipleEvents(Guest g, uint32_t count, uint32_t events, uint32_t wait_all, uint32_t timeout_ms,
                                    uint32_t thread);
  // XNet without a network: random bytes and keys work (they are local); connections, server lookups and QoS fail
  // as on a console that is not connected (WSAENETDOWN, a lost connection).
  uint32_t XNetRandom(Guest g, uint32_t buffer, uint32_t length);
  uint32_t XNetCreateKey(Guest g, uint32_t key_id, uint32_t key);
  uint32_t XNetQosRelease(uint32_t qos);
  uint32_t WsaSetLastError(uint32_t error);
  // WSASetEvent / WSAResetEvent: TRUE, or FALSE with the Win32 error of the failure.
  uint32_t WsaSetEvent(Guest g, uint32_t handle);
  uint32_t WsaResetEvent(Guest g, uint32_t handle);
  // Lookups that complete at once with no answer: a DNS record whose status is WSAENETDOWN, a QoS record with no
  // result (titles read it whatever the result); both from the kernel pool, given back by their release call, the
  // event set.
  uint32_t XNetDnsLookup(Guest g, uint32_t event, uint32_t dns_ptr);
  uint32_t XNetQosServiceLookup(Guest g, uint32_t event, uint32_t qos_ptr);
  // XNetGetOpt: the startup parameters XNetStartup was given (XNET_OPTID_STARTUP_PARAMS, 1); others WSAEINVAL.
  uint32_t XNetGetOpt(Guest g, uint32_t option, uint32_t buffer, uint32_t size_ptr);
  uint32_t XNetStartup(Guest g, uint32_t params);
  uint32_t LogonGetMachineId(Guest g, uint32_t id_ptr);

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
    // The profile enumerator's one item is written straight into the title's buffer (no host copy kept).
    bool local_profile = false;
  };
  struct Setting {
    uint32_t id;
    std::vector<uint8_t> data;  // 16 bytes of X_USER_PROFILE_SETTING_DATA, plus the binary / string bytes
  };
  Listener* FindListener(uint32_t handle);
  // A setting the title wrote: this run's, or a binary one kept in profile_root by an earlier run.
  std::vector<Setting>::const_iterator FindSetting(uint32_t id);
  // A setting kept (replacing one of its id), a binary one also written to profile_root.
  void PutSetting(Setting setting);
  // Completes an overlapped with a result, its extended error and a length.
  void CompleteEx(Guest g, uint32_t overlapped, uint32_t result, uint32_t extended, uint32_t length);
  Enumerator* FindEnumerator(uint32_t handle);
  std::filesystem::path ContentPath(Guest g, uint32_t content_data) const;
  std::filesystem::path ContentHeaderPath(const std::filesystem::path& package) const;
  uint32_t online_schema_ = 0;
  uint8_t dash_app_info_[12] = {};
  uint32_t dash_context_ = 0;
  uint64_t nui_session_id_ = 0;
  uint32_t user_preferences_ = 0;
  std::vector<std::string> profile_mounts_;  // the names XamProfileOpen mounted the profile under
  // The profile's host folder once made, and the content root it was made under: a profile opened again after
  // XamProfileClose is mounted without asking the host file system again.
  std::filesystem::path profile_dir_, profile_dir_root_;
  uint32_t language_fallback_ = 0;  // the 18 fallback locale names in the kernel heap, laid out at first use
  uint32_t language_fallback_at_[18] = {};
  uint32_t typeface_patch_at_[3] = {};  // the three font patch names in the kernel heap, laid out at first use
  // The SMC's state: the tray (X_DVD_TRAY_STATE: 0 open, 2 closed), the fan speeds (percent), the power LED and the
  // LEDs (state << 8 | the byte after it).
  uint8_t smc_tray_ = 2;
  uint32_t smc_cpu_fan_ = 100, smc_gpu_fan_ = 100;
  uint16_t smc_power_led_ = 0x0100, smc_leds_ = 0x8001;
  uint32_t iptv_name_ = 0;  // the IPTV provider's name in the kernel heap, laid out at first use
  uint32_t content_change_callback_ = 0, launch_request_callback_ = 0, nui_tilt_callback_ = 0;
  // The packages of a type as enumerator items of `item_size` bytes starting with their XCONTENT_DATA, sorted by name.
  std::vector<std::vector<uint8_t>> ContentItems(uint32_t device_id, uint32_t content_type, uint32_t item_size) const;

  XamHost host_;
  TitleSettings title_;
  // The close hook: a listener's or enumerator's state goes with its last handle.
  bool OnClose(uint32_t object);  // whether a listener or an enumerator was the object
  int close_hook_ = -1;          // OnClose's index among the object manager's close hooks
  std::vector<Listener> listeners_;
  std::vector<Enumerator> enumerators_;
  // The handle system modules answer by: a zeroed loader entry from the kernel heap, made at first use.
  uint32_t SystemModule(Guest g);
  uint32_t system_module_ = 0;
  std::vector<Setting> settings_;
  std::vector<uint32_t> terminate_notifications_;
  std::vector<std::string> open_content_;
  struct Pad {
    bool seen = false;
    GamepadState last{};
    uint32_t packet = 0;
    InputQueue input;
    InputMapper mapper;
  };
  std::array<Pad, 4> pads_{};
  std::mutex pads_mutex_;
  std::chrono::steady_clock::duration input_step_ = InputQueue::kConsoleFrame;
  InputScript input_script_;
  std::chrono::steady_clock::time_point input_script_start_{};
  std::thread sampler_;
  std::atomic<bool> sampling_{false};
  bool Connected(uint32_t user) const;
  // Samples the controller into its queue (pads_mutex_ held) unless the sampling thread does.
  void SampleIfPolled(uint32_t user, std::chrono::steady_clock::time_point at);
  void Sample(uint32_t user, std::chrono::steady_clock::time_point at);
  std::vector<uint8_t> launch_data_;
  bool launch_data_present_ = false;
  uint32_t cache_elements_ = 0;
  std::vector<uint8_t> startup_params_;
};

Xam& XamState();

}  // namespace any360::sys
