// The object manager: the handle table and the dispatcher objects (events, semaphores, mutants) with the NT kernel's
// semantics in the guest structures, as a decompiled title relies on them: the signal state lives in the object's
// dispatcher header, the Ke* routines return the previous state, handles start at 0xF8000000 and count by four.
// Objects a title creates through Nt* calls are allocated from the kernel heap the title hands the layer. Waits
// consume a signalled object immediately and otherwise poll (relative timeouts); the scheduler proper comes later.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "guest.h"

namespace any360::sys {

constexpr uint32_t kHandleBase = 0xF8000000, kInvalidHandle = 0xFFFFFFFF, kCurrentThreadHandle = 0xFFFFFFFE;
constexpr uint32_t kStatusTimeout = 0x102, kStatusObjectNameExists = 0x40000000, kStatusInvalidHandle = 0xC0000008,
                   kStatusInvalidParameter = 0xC000000D, kStatusMutantNotOwned = 0xC0000046,
                   kStatusSemaphoreLimitExceeded = 0xC000012B, kStatusNoMemory = 0xC0000017;

// Dispatcher header types.
enum ObjectType : uint8_t {
  kNotificationEvent = 0, kSynchronizationEvent = 1, kMutant = 2, kSemaphore = 5, kThread = 6,
  kNotificationTimer = 8, kSynchronizationTimer = 9
};

// Field offsets of the guest structures.
constexpr uint32_t kHeaderType = 0, kHeaderSignalState = 4, kHeaderWaitListFlink = 8, kHeaderWaitListBlink = 12;
constexpr uint32_t kEventSize = 16;
constexpr uint32_t kSemaphoreLimit = 16, kSemaphoreSize = 20;
constexpr uint32_t kMutantOwner = 24, kMutantAbandoned = 28, kMutantSize = 32;
constexpr uint32_t kTimerDueTime = 16, kTimerPeriod = 36, kTimerSize = 40;
constexpr uint32_t kWaitAll = 0, kWaitAny = 1;

// The guest clock: 100 ns units since 1601-01-01 (FILETIME), from the host clock.
uint64_t SystemTime();

class ObjectManager {
 public:
  // The guest heap the layer allocates kernel objects from (a title gives it a region it will not use).
  void SetKernelHeap(uint32_t begin, uint32_t size);
  void Reset();

  // Dispatcher routines on guest objects (Ke*). They return the previous signal state.
  void InitializeEvent(Guest g, uint32_t event, uint32_t type, uint32_t initial_state);
  uint32_t SetEvent(Guest g, uint32_t event);
  uint32_t ResetEvent(Guest g, uint32_t event);
  uint32_t PulseEvent(Guest g, uint32_t event);
  void InitializeSemaphore(Guest g, uint32_t semaphore, uint32_t count, uint32_t limit);
  // The previous count, or kStatusSemaphoreLimitExceeded in `status` when the release would pass the limit.
  uint32_t ReleaseSemaphore(Guest g, uint32_t semaphore, uint32_t adjustment, uint32_t& status);
  // Timers: a due time that is negative is relative (100 ns), positive absolute (system time); `period` in ms re-arms.
  // SetTimer returns whether the timer was already armed; expiry is noticed when the objects are polled (waits, Poll).
  void InitializeTimer(Guest g, uint32_t timer, uint32_t type, uint32_t process_type);
  uint32_t SetTimer(Guest g, uint32_t timer, int64_t due_time, uint32_t period_ms);
  uint32_t CancelTimer(Guest g, uint32_t timer);
  void Poll(Guest g);
  void InitializeMutant(Guest g, uint32_t mutant, uint32_t owner_thread);
  uint32_t ReleaseMutant(Guest g, uint32_t mutant, uint32_t current_thread, uint32_t& status);

  // Handles (Nt* / Ob*). `thread` is the current KTHREAD (from the PCR), used for mutants.
  uint32_t CreateEvent(Guest g, uint32_t handle_out, uint32_t attributes, uint32_t type, uint32_t initial_state);
  uint32_t CreateSemaphore(Guest g, uint32_t handle_out, uint32_t attributes, uint32_t count, uint32_t limit);
  uint32_t CreateMutant(Guest g, uint32_t handle_out, uint32_t attributes, uint32_t initial_owner, uint32_t thread);
  uint32_t NtSetEvent(Guest g, uint32_t handle, uint32_t previous_out);
  uint32_t NtPulseEvent(Guest g, uint32_t handle, uint32_t previous_out);
  uint32_t NtClearEvent(Guest g, uint32_t handle);
  uint32_t NtQueryEvent(Guest g, uint32_t handle, uint32_t out);
  uint32_t NtReleaseSemaphore(Guest g, uint32_t handle, uint32_t count, uint32_t previous_out);
  uint32_t NtReleaseMutant(Guest g, uint32_t handle, uint32_t thread);
  uint32_t CreateTimer(Guest g, uint32_t handle_out, uint32_t attributes, uint32_t type);
  uint32_t NtSetTimerEx(Guest g, uint32_t handle, uint32_t due_time_ptr, uint32_t period_ms, uint32_t previous_out);
  uint32_t NtCancelTimer(Guest g, uint32_t handle, uint32_t current_state_out);
  uint32_t Close(uint32_t handle);
  uint32_t Duplicate(Guest g, uint32_t handle, uint32_t new_handle_out, uint32_t options);
  // Writes the object's guest address; the current-thread pseudo handle resolves to `thread`.
  uint32_t ReferenceObjectByHandle(Guest g, uint32_t handle, uint32_t thread, uint32_t object_out);
  uint32_t DereferenceObject(Guest g, uint32_t object);

  // Waits. A null timeout waits for ever; a relative timeout (negative, 100 ns units) polls that long; zero does not
  // wait. Returns STATUS_SUCCESS when the object was (or became) signalled and consumed, STATUS_TIMEOUT otherwise.
  uint32_t WaitForObject(Guest g, uint32_t object, uint32_t thread, const int64_t* timeout);
  uint32_t WaitForHandle(Guest g, uint32_t handle, uint32_t thread, const int64_t* timeout);
  // Several objects: wait-any returns the index of the object taken, wait-all 0; STATUS_TIMEOUT otherwise.
  uint32_t WaitForObjects(Guest g, uint32_t count, const uint32_t* objects, uint32_t wait_type, uint32_t thread,
                          const int64_t* timeout);
  // The handles are read from guest memory; an unknown one is STATUS_INVALID_PARAMETER.
  uint32_t WaitForHandles(Guest g, uint32_t count, uint32_t handles, uint32_t wait_type, uint32_t thread,
                          const int64_t* timeout);

  // The object a handle names (0 when none), for tests and binders.
  uint32_t Lookup(uint32_t handle);
  // For the other managers: a guest region from the kernel heap (0 when exhausted), a handle on an existing object,
  // the guest memory the heap lives in.
  uint32_t AllocateObject(uint32_t size) { return Allocate(size); }
  uint32_t OpenHandle(uint32_t object) { return AddHandle(object, {}); }
  Guest guest() const { return guest_; }
  void SetGuest(Guest g) { guest_ = g; }

 private:
  // One handle: a slot of the table, free again once closed. Ob* references are counted per object apart from
  // handles; objects stay allocated either way (the kernel heap does not reclaim yet).
  struct Entry {
    uint32_t object = 0;  // guest address of the dispatcher structure
    bool open = false;
    std::string name;
  };
  uint32_t Allocate(uint32_t size);
  uint32_t AddHandle(uint32_t object, const std::string& name);
  Entry* Find(uint32_t handle);
  Entry* FindByName(const std::string& name);
  std::string NameOf(Guest g, uint32_t attributes) const;
  bool TrySatisfy(Guest g, uint32_t object, uint32_t thread);
  bool IsSatisfiable(Guest g, uint32_t object, uint32_t thread) const;

  std::vector<Entry> entries_;
  std::unordered_map<uint32_t, int> references_;  // object -> Ob references
  std::vector<uint32_t> timers_;  // armed timers (guest addresses)
  uint32_t heap_begin_ = 0, heap_end_ = 0, heap_next_ = 0;
  Guest guest_{nullptr};
};

ObjectManager& Objects();

}  // namespace any360::sys
