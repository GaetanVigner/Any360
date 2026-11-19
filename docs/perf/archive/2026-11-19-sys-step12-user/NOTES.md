# Step 12, the calls Xenia stubs too: the 11 profile and user calls

`XamProfileClose`, `XamProfileCreate`, `XamProfileGetCreationStatus`, `XamProfileLoadAccountInfo`,
`XamUserCreateTitlesPlayedEnumerator`, `XamUserIsGuest`, `XamUserIsUnsafeProgrammingAllowed`, `XamUserLogonEx`,
`XamUserNuiEnableBiometric`, `XamUserNuiGetUserIndex`, `XamUserNuiGetUserIndexForBind`, answered as Xenia's
(xam_profile.cc, xam_user.cc, xam_nui.cc). Measured on 2026-11-19 (AMD Ryzen 7 7800X3D, cores 0-7, the machine
otherwise quiet) with `sys_abi_bench 9`, three times (`sys_abi_bench_run1-3.txt`); the first measure, before the two
changes below, in `before/`. Every row's result is checked after its timing. No host call does this work.

Reused, not recoded: the profile's mount (`XamProfileOpen`'s), `XamContentClose`'s unmount, the account
`XamProfileFindAccount` writes, the enumerator objects (`CreateEnumerator`), the overlapped completion (`Finish`).

| call | original ns | Any360 ns | speed | B, allocs a call |
|---|---|---|---|---|
| XamUserIsGuest, XamUserNuiEnableBiometric, XamUserNuiGetUserIndex, XamUserNuiGetUserIndexForBind | 65-69 | 2.0-2.2 | 31-34x | 0, 0 |
| XamProfileCreate | 67-68 | 3.7 | 18x | 0, 0 |
| XamUserLogonEx (no overlapped) | 66 | 3.8-4.0 | 17-18x | 0, 0 |
| XamUserIsUnsafeProgrammingAllowed | 66-67 | 4.0 | 17x | 0, 0 |
| XamProfileGetCreationStatus | 66 | 4.1-4.3 | 16x | 0, 0 |
| XamProfileClose (nothing mounted under the name) | 66 | 12.8-13.4 | 5.0-5.2x | 0, 0 |
| XamProfileLoadAccountInfo | 65-66 | 13.7-13.8 | 4.8x | 0, 0 |
| XamUserCreateTitlesPlayedEnumerator + NtClose | 68-69 | 112-113 | 0.6x | 0, 0 |
| XamProfileOpen + XamProfileClose (mounted, unmounted) | 134-135 | 523-635 | 0.2-0.3x | 568, 10 |

The original is the reference runtime's stub: it creates no enumerator and mounts nothing. The two slower rows are
work the stub does not do. The enumerator row is a kernel object made and closed, the same cost as
`XamProfileCreateEnumerator + NtClose` (106-110 ns). The open and close row is a device mounted and linked, then
removed: its strings and the mount entry are the 10 allocations. A title opens its profile once or twice a session.

- **The profile's host folder**: `XamProfileOpen` after `XamProfileClose` asked the host to create the folder again
  (`create_directories`): 610-679 us a cycle with 14 allocations (`before/`). The folder made, and its content root,
  are now kept, so a reopen only mounts: 0.52-0.63 us and 10 allocations. Kept.
- **The enumerator list**: `Xam::OnClose` and `FindEnumerator` searched it from the front (`erase_if` scanned it
  all). The bench's profile-enumerator row left its enumerators open, about 2,000 of them, ahead of every later row:
  the titles-played cycle measured 773-791 ns (`before/`). Both now search from the end, where a title's newest
  enumerator is (create, enumerate, close): with the bench row unchanged, the titles-played cycle 113 ns, but
  `XamProfileEnumerate` on the oldest enumerator, now behind the 2,000 newer ones, 670 ns
  (`reverse-search-only/`): the order only moves the cost when a title leaves enumerators open. The bench row now
  closes what it opens (`+ NtClose`, as the other enumerator rows do): titles-played 112 ns, `XamProfileEnumerate`
  at the end of its enumerator 15-16 ns.

The answers: `XamProfileClose` unmounts a profile `XamProfileOpen` mounted, or content opened under that name
(Xenia's `CloseContent`), else ERROR_FUNCTION_FAILED. Any360 keeps one local profile and makes no other, as
`XamShowCreateProfileUI` does. So `XamProfileCreate` fails the way Xenia's does when its profile manager cannot create
a profile: ERROR_INVALID_PARAMETER, the device out set to 1. Unlike Xenia, which allocates the creation info before
it fails (and keeps it), Any360 hands out none: the info out is set to 0. `XamProfileGetCreationStatus` copies a given
info's offline XUID (at 0xA78, after the flags, the device, the account and the payment info) and frees nothing, since Any360 hands none out. `XamProfileLoadAccountInfo` writes the
local profile's account and gives ERROR_INVALID_PARAMETER for XUID 0, another XUID or no account out. The
titles-played enumerator is empty: Xenia lists only titles with achievements, and the profile has none. Its items
are XTITLE_PLAYED (0xA8 bytes) and its buffer size is count * 0xA8. It gives ERROR_INVALID_PARAMETER for a user past 3
or no count, size or handle out, and ERROR_FUNCTION_FAILED for a user not signed in. There is no guest. Unsafe
programming is allowed (1) for users 0-3 and XUSER_INDEX_ANY. `XamUserLogonEx` succeeds, through the overlapped when
one is given. There is no Kinect user: EnableBiometric E_INVALIDARG, GetUserIndex the NO_SUCH_USER HRESULT, and
GetUserIndexForBind E_FAIL (E_INVALIDARG without an index out).

Checked by `sys_adapter_tests` (`UserAdapters`): each adapter called with its arguments in the registers a title
uses, with r3 and the guest bytes checked. The profile's mount is resolved after the open and gone after the close,
and a second close fails. `XamProfileClose` also closes content `XamContentCreate` opened under the name. The account is compared byte for byte
with `XamProfileFindAccount`'s. The titles-played
enumerator ends at its first `XamEnumerate` (ERROR_NO_MORE_FILES) and closes. `XamUserLogonEx` completes an overlapped.
