# Step 12, tenth batch: ObCreateObject and XexLoadImageFromMemory

The last two calls Xenia implements and the reference runtime stubs (docs/SYS_STUBS.md). Measured on 2026-11-19
(AMD Ryzen 7 7800X3D, cores 0-7, another session's test suite on cores 8-15) with `sys_abi_bench 9` and
`sys_step12_bench 9`, three times each (`sys_abi_bench_run1-3.txt`, `sys_step12_bench_run1-3.txt`); every row's
result is checked after its timing (none differed).

In the ABI bench the object type's allocate procedure (guest code) is played by the scheduler's dispatcher, which
returns the same block every call: the rows time the kernel's part, as a title's allocator would add its own.

Against the original (the reference's stub, 67-70 ns a call, doing none of the work):

| call | original ns | Any360 ns | speed | B, allocs a call |
|---|---|---|---|---|
| ObCreateObject, unnamed, 0x40 bytes | 68-69 | 10.2-11.7 | 5.8-6.8x | 0, 0 |
| ObCreateObject, named ("\Title\Event7"), 0x40 bytes | 68-70 | 18.9-19.8 | 3.4-3.7x | 0, 0 |
| XexLoadImageFromMemory, a module not loaded | 67-68 | 24.8-25.0 | 2.7x | 0, 0 |

Without the ABI (`sys_step12_bench`, the allocate procedure a host function): ObCreateObject 4.9 ns unnamed, 13.3-13.9
ns named; XexLoadImageFromMemory 23.4-24.8 ns (the name compared against the loaded modules as XexLoadImage does).
No host call does this work.

The answers (Xenia's xboxkrnl_ob.cc and xboxkrnl_modules.cc):

- `ObCreateObject`: allocate(size + 0x10, the type's pool tag, the pool) in guest code; the OBJECT_HEADER (pointer
  count 1, handle count 0, the type, the flags; its hash index left as allocated); the pool and the title-object flag
  (0x10) from the attributes (0x1000: title, 0x2000: system) or the calling thread's process type (KTHREAD +0x73, a
  title's: 1); the object written, 0 on failure; STATUS_INSUFFICIENT_RESOURCES. Named objects: Xenia stops on them
  (its name splitting is not written), so its structures' layout with NT's split: the OBJECT_HEADER_NAME_INFO (no
  directory, the name's length twice, its pointer), the header with the named flag (1), the object (its size rounded
  to 4), the last path component's copy; an empty component (no name, a lone, doubled or trailing backslash) is
  STATUS_OBJECT_NAME_INVALID before any allocation. No object directory is entered (Xenia enters none either).
- `XexLoadImageFromMemory`: no handle pointer is ERROR_INVALID_PARAMETER (0x57, a Win32 code, as Xenia returns it); a
  loaded module's name (the title's, xam.xex, xboxkrnl.exe; a path's directories ignored) writes 0 and is
  STATUS_OBJECT_NAME_COLLISION; any other image would be new code, which a statically recompiled title cannot run:
  0 and STATUS_NO_SUCH_FILE, Xenia's answer when an image does not load.

Checked by `sys_object_tests` (`ObStep12TenthTests`): the pool and flag for every attribute and process case, the
allocate procedure's arguments, the header, a named object's layout and its size alignment, a name without a
directory, five invalid names (nothing allocated), failed allocations named or not, no host allocation; the module
names that collide, a module not loaded, no name, no handle pointer. Not covered: the allocate procedure through the
real dispatcher on a guest thread (the tests use a host function, as LDI's do); a title calling these in game.
