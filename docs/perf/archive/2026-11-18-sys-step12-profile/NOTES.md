# Step 12, sixth batch: 17 profile and user calls

The calls Xenia implements and the reference runtime stubs (docs/SYS_STUBS.md, "user"): `XamProfileCreateEnumerator`,
`XamProfileEnumerate`, `XamProfileFindAccount`, `XamProfileGetLiveLegalLocale`, `XamProfileOpen`,
`XamUserGetCachedUserFlags`, `XamUserGetIndexFromXUID`, `XamUserGetOnlineLanguageFromXUID`,
`XamUserGetSubscriptionType`, `XamUserGetUserFlags`, `XamUserGetUserFlagsFromXUID`, `XamUserGetUserTenure`,
`XamUserIsParentalControlled`, `XamUserLogon`, `XamUserNuiGetUserIndexForSignin`, `XamUserReadUserPreference`,
`XamUserWriteUserPreference`. Measured on 2026-11-18 with `sys_abi_bench 9` (the original through the PowerPC ABI)
and `sys_step12_bench 9`, three times each. Raw output: `sys_abi_bench_run1-3.txt`, `sys_step12_bench_run1-3.txt`.

Reused, not recoded: the local profile (TitleSettings' XUID and gamertag) and its sign-in state, the console's
language and country (`Console()`), `XamGetOnlineLanguageAndCountryString` for the legal locale, XAM's enumerator and
its overlapped completion, the content mounts (`Files().Mount` / `Link`, as XamContentCreate), the notification queue.

Against the original (the reference's stub, 64-85 ns a call; Any360 ns and speed over three runs):

| call | Any360 ns | speed |
|---|---|---|
| XamUserIsParentalControlled, XamUserGetOnlineLanguageFromXUID | 2.2-3.0 | 23-30x |
| XamUserGetIndexFromXUID, XamUserGetSubscriptionType, XamUserGetUserTenure, XamUserLogon, XamUserNuiGetUserIndexForSignin | 3.7-4.7 | 14-19x |
| XamUserReadUserPreference, XamUserWriteUserPreference | 5.0-6.0 | 11-13x |
| XamUserGetUserFlags, XamUserGetCachedUserFlags, XamUserGetUserFlagsFromXUID | 5.7-6.6 | 10-12x |
| XamProfileFindAccount (0x17C bytes written) | 13.9-17.0 | 4.7-4.8x |
| XamProfileEnumerate (at the end of the list) | 15.6-16.6 | 4.1-4.6x |
| XamProfileGetLiveLegalLocale | 19.2-24.3 | 3.0-3.5x |
| XamProfileOpen (the profile already mounted) | 65.7-66.3 | 1.0-1.1x |
| XamProfileCreateEnumerator (each left open) | 262-361 | 0.2-0.3x |

- **XamProfileCreateEnumerator** makes a kernel object and a handle each call (the object manager's work: a block of
  the kernel heap, a handle, and its bookkeeping), where the stub makes nothing. As a title uses it (create, read the
  profile, read the end, close) the whole cycle is 142-164 ns with 1 allocation of 24 bytes (`sys_step12_bench`):
  the object manager's node for an object freed on close. Left open (the ABI row), each also grows XAM's enumerator
  list: 2 allocations, 339 bytes.
- **Fixed during the batch** (measured, then changed): `XamProfileOpen` took 630 µs a call and 9 allocations: it
  created the host folder and remounted it every time. It now mounts a name once (the title finds its profile as it
  left it): 66 ns, 0 allocations. `XamProfileCreateEnumerator` kept a host copy of the profile item (5 allocations a
  call, 230-280 ns a cycle): the enumerator now writes the item straight into the title's buffer. And
  `Files().Mount` of a device already mounted now replaces it instead of listing it twice (a title opening the same
  content or profile again grew the mount list).
- **Against the host**: nothing on the host does this work (the console's profiles); no host column.
- **Host memory**: 0 B and 0 allocations a call for every row but the enumerator's creation above.

The answers (Xenia's xam_profile.cc, xam_user.cc, xam_enum.cc, user_tracker.cc), with one local profile signed in
as user 0:

- The account: the gamertag, no online XUID, no reserved flags, the console's country and language in the cached user
  flags (Xenia keeps both at 0 in a new account and answers from the console's settings instead), no subscription
  tier, no parental controls. User flags are the cached flags for the local profile, 0 for any other.
- `XamProfileOpen` mounts the profile's folder (under the content root) as `<name>:` (`User_<XUID>` without a name)
  and fills the content data (profile type, device 1, the XUID as file name, the dashboard's title id);
  another XUID is refused.
- Preferences: Xenia's rules on the dashboard's XPROFILE_USER_PREFERENCES word (a mask names one preference, two of
  them with a "written" bit; S_FALSE when never written; a write sets the written bit and notifies
  XN_SYS_PROFILESETTINGCHANGED), kept for the run (not saved between runs yet).
- `XamUserLogon` succeeds and changes nothing: the one local profile stays signed in (Any360 has no other profile and
  no signed-out state); tenure and subscription: none recorded.

Checked by `sys_object_tests` (`ProfileStep12Tests`): every value and byte (the account, the enumeration and its end,
the content data, the mounted folder), every refusal (other XUIDs and users, null outputs), the preference rules with
their extended errors through an overlapped and the notification.
