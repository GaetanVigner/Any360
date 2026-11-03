#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace any360::sys {

namespace {

constexpr uint32_t kStatusSuccess = 0;

void InitializeHeader(Guest g, uint32_t object, uint8_t type, uint32_t signal_state) {
  g.Store8(object + kHeaderType, type);
  g.Store8(object + 1, 0);
  g.Store8(object + 2, 0);
  g.Store8(object + 3, 0);
  g.Store32(object + kHeaderSignalState, signal_state);
  // An empty wait list: the list head points at itself.
  g.Store32(object + kHeaderWaitListFlink, object + kHeaderWaitListFlink);
  g.Store32(object + kHeaderWaitListBlink, object + kHeaderWaitListFlink);
}

// How long a wait may last: for ever without a timeout, a relative timeout's length (negative, 100 ns units), or the
// time left until an absolute one (a system time; zero once it has passed).
std::chrono::nanoseconds WaitLimit(const int64_t* timeout) {
  constexpr auto kForever = std::chrono::nanoseconds::max();
  if (!timeout) return kForever;
  if (*timeout <= 0) return std::chrono::nanoseconds(-*timeout * 100);
  const uint64_t now = SystemTime(), due = uint64_t(*timeout);
  if (due <= now) return std::chrono::nanoseconds(0);
  return due - now > uint64_t(kForever.count() / 100) ? kForever : std::chrono::nanoseconds((due - now) * 100);
}

}  // namespace

uint64_t SystemTime() {
  // 1601-01-01 to 1970-01-01 in 100 ns units.
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count() / 100) + 116444736000000000ull;
}

ObjectManager& Objects() {
  static ObjectManager objects;
  return objects;
}

void ObjectManager::SetKernelHeap(uint32_t begin, uint32_t size) {
  heap_begin_ = heap_next_ = begin;
  heap_end_ = begin + size;
}

void ObjectManager::Reset() {
  entries_.clear();
  references_.clear();
  timers_.clear();
  timer_apcs_.clear();
  free_blocks_.clear();
  freeable_.clear();
  heap_next_ = heap_begin_;
}

uint32_t ObjectManager::Allocate(uint32_t size, bool freed_on_close) {
  const uint32_t aligned = (size + 15) & ~15u;
  std::lock_guard lock(lifetime_mutex_);
  uint32_t at = 0;
  // First fit among the freed blocks, then the untouched rest of the heap.
  for (auto it = free_blocks_.begin(); it != free_blocks_.end(); ++it) {
    if (it->second < aligned) continue;
    at = it->first;
    const uint32_t left = it->second - aligned;
    free_blocks_.erase(it);
    if (left) free_blocks_[at + aligned] = left;
    break;
  }
  if (!at) {
    if (heap_next_ + aligned > heap_end_) return 0;
    at = heap_next_;
    heap_next_ += aligned;
  }
  if (freed_on_close) freeable_[at] = aligned;
  return at;
}

uint32_t ObjectManager::AllocateObject(uint32_t size, bool freed_on_close) { return Allocate(size, freed_on_close); }

void ObjectManager::ReferenceObject(uint32_t object) {
  std::lock_guard lock(lifetime_mutex_);
  ++references_[object];
}

void ObjectManager::Release(uint32_t object) {
  std::lock_guard lock(lifetime_mutex_);
  const auto owned = freeable_.find(object);
  if (owned == freeable_.end() || references_.count(object)) return;
  for (const Entry& e : entries_) {
    if (e.open && e.object == object) return;
  }
  timers_.erase(std::remove(timers_.begin(), timers_.end(), object), timers_.end());
  timer_apcs_.erase(object);
  uint32_t at = object, size = owned->second;
  freeable_.erase(owned);
  // Merged with the free blocks on either side; one that reaches the untouched rest of the heap joins it.
  if (auto next = free_blocks_.find(at + size); next != free_blocks_.end()) {
    size += next->second;
    free_blocks_.erase(next);
  }
  if (auto next = free_blocks_.lower_bound(at); next != free_blocks_.begin()) {
    auto previous = std::prev(next);
    if (previous->first + previous->second == at) {
      at = previous->first;
      size += previous->second;
      free_blocks_.erase(previous);
    }
  }
  if (at + size == heap_next_) {
    heap_next_ = at;
  } else {
    free_blocks_[at] = size;
  }
}

uint32_t ObjectManager::AddHandle(uint32_t object, const std::string& name) {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (!entries_[i].open) {
      entries_[i] = {object, true, name};
      return kHandleBase + uint32_t(i) * 4;
    }
  }
  entries_.push_back({object, true, name});
  return kHandleBase + uint32_t(entries_.size() - 1) * 4;
}

ObjectManager::Entry* ObjectManager::Find(uint32_t handle) {
  if (handle < kHandleBase || (handle & 3)) return nullptr;
  const uint32_t index = (handle - kHandleBase) / 4;
  if (index >= entries_.size() || !entries_[index].open) return nullptr;
  return &entries_[index];
}

ObjectManager::Entry* ObjectManager::FindByName(const std::string& name) {
  if (name.empty()) return nullptr;
  for (Entry& e : entries_) {
    if (e.open && e.name == name) return &e;
  }
  return nullptr;
}

// OBJECT_ATTRIBUTES: root directory, a pointer to an ANSI_STRING name, attributes.
std::string ObjectManager::NameOf(Guest g, uint32_t attributes) const {
  if (!attributes) return {};
  const uint32_t string = g.Load32(attributes + 4);
  if (!string) return {};
  const uint16_t length = g.Load16(string);
  const uint32_t pointer = g.Load32(string + 4);
  if (!pointer || !length) return {};
  return std::string(reinterpret_cast<const char*>(g.host(pointer)), length);
}

uint32_t ObjectManager::Lookup(uint32_t handle) {
  const Entry* e = Find(handle);
  return e ? e->object : 0;
}

// --- dispatcher routines ---------------------------------------------------------------------------------------

void ObjectManager::InitializeEvent(Guest g, uint32_t event, uint32_t type, uint32_t initial_state) {
  InitializeHeader(g, event, uint8_t(type), initial_state ? 1 : 0);
}

uint32_t ObjectManager::SetEvent(Guest g, uint32_t event) {
  const uint32_t previous = g.Load32(event + kHeaderSignalState);
  g.Store32(event + kHeaderSignalState, 1);  // a waiter would consume a synchronization event here
  return previous;
}

uint32_t ObjectManager::ResetEvent(Guest g, uint32_t event) {
  const uint32_t previous = g.Load32(event + kHeaderSignalState);
  g.Store32(event + kHeaderSignalState, 0);
  return previous;
}

uint32_t ObjectManager::PulseEvent(Guest g, uint32_t event) {
  const uint32_t previous = g.Load32(event + kHeaderSignalState);
  g.Store32(event + kHeaderSignalState, 0);  // waiters are released, the event ends unsignalled
  return previous;
}

void ObjectManager::InitializeSemaphore(Guest g, uint32_t semaphore, uint32_t count, uint32_t limit) {
  InitializeHeader(g, semaphore, kSemaphore, count);
  g.Store32(semaphore + kSemaphoreLimit, limit);
}

uint32_t ObjectManager::ReleaseSemaphore(Guest g, uint32_t semaphore, uint32_t adjustment, uint32_t& status) {
  const uint32_t previous = g.Load32(semaphore + kHeaderSignalState);
  const uint32_t limit = g.Load32(semaphore + kSemaphoreLimit);
  if (adjustment > limit || previous > limit - adjustment) {
    status = kStatusSemaphoreLimitExceeded;
    return previous;
  }
  g.Store32(semaphore + kHeaderSignalState, previous + adjustment);
  status = kStatusSuccess;
  return previous;
}

void ObjectManager::InitializeTimer(Guest g, uint32_t timer, uint32_t type, uint32_t process_type) {
  // The other fields carry through calls; the header, due time and period are reset.
  g.Store8(timer + kHeaderType, uint8_t(kNotificationTimer + (type & 1)));
  g.Store8(timer + 2, uint8_t(process_type));
  g.Store8(timer + 3, 0);  // inserted
  g.Store32(timer + kHeaderSignalState, 0);
  g.Store32(timer + kHeaderWaitListFlink, timer + kHeaderWaitListFlink);
  g.Store32(timer + kHeaderWaitListBlink, timer + kHeaderWaitListFlink);
  g.Store64(timer + kTimerDueTime, 0);
  g.Store32(timer + kTimerPeriod, 0);
}

uint32_t ObjectManager::SetTimer(Guest g, uint32_t timer, int64_t due_time, uint32_t period_ms) {
  const bool was_armed = std::find(timers_.begin(), timers_.end(), timer) != timers_.end();
  const uint64_t due = due_time < 0 ? SystemTime() + uint64_t(-due_time) : uint64_t(due_time);
  g.Store64(timer + kTimerDueTime, due);
  g.Store32(timer + kTimerPeriod, period_ms);
  g.Store32(timer + kHeaderSignalState, 0);
  g.Store8(timer + 3, 1);
  if (!was_armed) timers_.push_back(timer);
  return was_armed ? 1 : 0;
}

uint32_t ObjectManager::CancelTimer(Guest g, uint32_t timer) {
  auto it = std::find(timers_.begin(), timers_.end(), timer);
  if (it == timers_.end()) return 0;
  timers_.erase(it);
  g.Store8(timer + 3, 0);
  return 1;
}

void ObjectManager::Poll(Guest g) {
  const uint64_t now = SystemTime();
  for (size_t i = 0; i < timers_.size();) {
    const uint32_t timer = timers_[i];
    if (g.Load64(timer + kTimerDueTime) > now) {
      ++i;
      continue;
    }
    g.Store32(timer + kHeaderSignalState, 1);
    if (const auto apc = timer_apcs_.find(timer); apc != timer_apcs_.end()) {
      const TimerApc& a = apc->second;
      Threads().QueueApcTo(a.thread, a.routine, a.context, uint32_t(now), uint32_t(now >> 32));
    }
    const uint32_t period = g.Load32(timer + kTimerPeriod);
    if (period) {
      g.Store64(timer + kTimerDueTime, now + uint64_t(period) * 10'000);
      ++i;
    } else {
      g.Store8(timer + 3, 0);
      timers_.erase(timers_.begin() + long(i));
    }
  }
}

void ObjectManager::InitializeMutant(Guest g, uint32_t mutant, uint32_t owner_thread) {
  InitializeHeader(g, mutant, kMutant, owner_thread ? 0 : 1);
  g.Store32(mutant + 16, mutant + 16);  // mutant list entry, unlinked
  g.Store32(mutant + 20, mutant + 16);
  g.Store32(mutant + kMutantOwner, owner_thread);
  g.Store8(mutant + kMutantAbandoned, 0);
  g.Store8(mutant + kMutantAbandoned + 1, 0);
}

uint32_t ObjectManager::ReleaseMutant(Guest g, uint32_t mutant, uint32_t current_thread, uint32_t& status) {
  const uint32_t previous = g.Load32(mutant + kHeaderSignalState);
  if (g.Load32(mutant + kMutantOwner) != current_thread) {
    status = kStatusMutantNotOwned;
    return previous;
  }
  const uint32_t state = previous + 1;
  g.Store32(mutant + kHeaderSignalState, state);
  if (state == 1) g.Store32(mutant + kMutantOwner, 0);
  status = kStatusSuccess;
  return previous;
}

// --- handles ---------------------------------------------------------------------------------------------------

uint32_t ObjectManager::CreateEvent(Guest g, uint32_t handle_out, uint32_t attributes, uint32_t type, uint32_t initial_state) {
  const std::string name = NameOf(g, attributes);
  if (Entry* existing = FindByName(name)) {
    const uint8_t t = g.Load8(existing->object + kHeaderType);
    if (t != kNotificationEvent && t != kSynchronizationEvent) return kStatusInvalidHandle;
    if (handle_out) g.Store32(handle_out, AddHandle(existing->object, existing->name));
    return kStatusObjectNameExists;
  }
  const uint32_t object = Allocate(kEventSize, true);
  if (!object) return kStatusNoMemory;
  InitializeEvent(g, object, type ? kSynchronizationEvent : kNotificationEvent, initial_state);
  const uint32_t handle = AddHandle(object, name);
  if (handle_out) g.Store32(handle_out, handle);
  return kStatusSuccess;
}

uint32_t ObjectManager::CreateSemaphore(Guest g, uint32_t handle_out, uint32_t attributes, uint32_t count, uint32_t limit) {
  const std::string name = NameOf(g, attributes);
  if (Entry* existing = FindByName(name)) {
    if (g.Load8(existing->object + kHeaderType) != kSemaphore) return kStatusInvalidHandle;
    if (handle_out) g.Store32(handle_out, AddHandle(existing->object, existing->name));
    return kStatusObjectNameExists;
  }
  if (count > limit) return kStatusInvalidParameter;
  const uint32_t object = Allocate(kSemaphoreSize, true);
  if (!object) return kStatusNoMemory;
  InitializeSemaphore(g, object, count, limit);
  const uint32_t handle = AddHandle(object, name);
  if (handle_out) g.Store32(handle_out, handle);
  return kStatusSuccess;
}

uint32_t ObjectManager::CreateMutant(Guest g, uint32_t handle_out, uint32_t attributes, uint32_t initial_owner, uint32_t thread) {
  const std::string name = NameOf(g, attributes);
  if (Entry* existing = FindByName(name)) {
    if (g.Load8(existing->object + kHeaderType) != kMutant) return kStatusInvalidHandle;
    if (handle_out) g.Store32(handle_out, AddHandle(existing->object, existing->name));
    return kStatusObjectNameExists;
  }
  const uint32_t object = Allocate(kMutantSize, true);
  if (!object) return kStatusNoMemory;
  InitializeMutant(g, object, initial_owner ? thread : 0);
  const uint32_t handle = AddHandle(object, name);
  if (handle_out) g.Store32(handle_out, handle);
  return kStatusSuccess;
}

uint32_t ObjectManager::NtSetEvent(Guest g, uint32_t handle, uint32_t previous_out) {
  const Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  const uint32_t previous = SetEvent(g, e->object);
  if (previous_out) g.Store32(previous_out, previous);
  return kStatusSuccess;
}

uint32_t ObjectManager::NtPulseEvent(Guest g, uint32_t handle, uint32_t previous_out) {
  const Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  const uint32_t previous = PulseEvent(g, e->object);
  if (previous_out) g.Store32(previous_out, previous);
  return kStatusSuccess;
}

uint32_t ObjectManager::NtClearEvent(Guest g, uint32_t handle) {
  const Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  ResetEvent(g, e->object);
  return kStatusSuccess;
}

uint32_t ObjectManager::NtQueryEvent(Guest g, uint32_t handle, uint32_t out) {
  const Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  g.Store32(out, g.Load8(e->object + kHeaderType));
  g.Store32(out + 4, g.Load32(e->object + kHeaderSignalState));
  return kStatusSuccess;
}

uint32_t ObjectManager::NtReleaseSemaphore(Guest g, uint32_t handle, uint32_t count, uint32_t previous_out) {
  const Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  uint32_t status;
  const uint32_t previous = ReleaseSemaphore(g, e->object, count, status);
  if (previous_out) g.Store32(previous_out, previous);
  return status;
}

uint32_t ObjectManager::NtReleaseMutant(Guest g, uint32_t handle, uint32_t thread) {
  const Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  uint32_t status;
  ReleaseMutant(g, e->object, thread, status);
  return status;
}

uint32_t ObjectManager::CreateTimer(Guest g, uint32_t handle_out, uint32_t attributes, uint32_t type) {
  const std::string name = NameOf(g, attributes);
  if (Entry* existing = FindByName(name)) {
    const uint8_t t = g.Load8(existing->object + kHeaderType);
    if (t != kNotificationTimer && t != kSynchronizationTimer) return kStatusInvalidHandle;
    if (handle_out) g.Store32(handle_out, AddHandle(existing->object, existing->name));
    return kStatusObjectNameExists;
  }
  const uint32_t object = Allocate(kTimerSize, true);
  if (!object) return kStatusNoMemory;
  InitializeTimer(g, object, type, 0);
  const uint32_t handle = AddHandle(object, name);
  if (handle_out) g.Store32(handle_out, handle);
  return kStatusSuccess;
}

uint32_t ObjectManager::NtSetTimerEx(Guest g, uint32_t handle, uint32_t due_time_ptr, uint32_t routine, uint32_t context,
                                    uint32_t thread, uint32_t period_ms, uint32_t previous_out) {
  const Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  if (!due_time_ptr) return kStatusInvalidParameter;
  if (routine) {
    timer_apcs_[e->object] = {thread, routine, context};
  } else {
    timer_apcs_.erase(e->object);
  }
  const uint32_t previous = SetTimer(g, e->object, int64_t(g.Load64(due_time_ptr)), period_ms);
  if (previous_out) g.Store32(previous_out, previous);
  return kStatusSuccess;
}

uint32_t ObjectManager::NtCancelTimer(Guest g, uint32_t handle, uint32_t current_state_out) {
  const Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  const uint32_t state = CancelTimer(g, e->object);
  if (current_state_out) g.Store32(current_state_out, state);
  return kStatusSuccess;
}

void ObjectManager::Closed(uint32_t object) {
  if (!close_hook_) return;
  for (const Entry& e : entries_) {
    if (e.open && e.object == object) return;
  }
  close_hook_(object);
}

uint32_t ObjectManager::Close(uint32_t handle) {
  Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  e->open = false;
  const uint32_t object = e->object;
  Closed(object);
  Release(object);
  return kStatusSuccess;
}

uint32_t ObjectManager::Duplicate(Guest g, uint32_t handle, uint32_t new_handle_out, uint32_t options) {
  Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  const uint32_t duplicate = AddHandle(e->object, e->name);
  e = Find(handle);  // the table may have grown
  if (new_handle_out) g.Store32(new_handle_out, duplicate);
  if (options == 1 && e) e->open = false;  // DUPLICATE_CLOSE_SOURCE (the duplicate keeps the object open)
  return kStatusSuccess;
}

uint32_t ObjectManager::ReferenceObjectByHandle(Guest g, uint32_t handle, uint32_t thread, uint32_t object_out) {
  if (handle == kCurrentThreadHandle) {
    if (object_out) g.Store32(object_out, thread);
    return kStatusSuccess;
  }
  Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  ReferenceObject(e->object);
  if (object_out) g.Store32(object_out, e->object);
  return kStatusSuccess;
}

uint32_t ObjectManager::OpenObjectByName(Guest g, uint32_t attributes, uint32_t handle_out) {
  constexpr uint32_t kStatusObjectNameNotFound = 0xC0000034;
  if (!attributes) return kStatusInvalidParameter;
  const Entry* e = FindByName(NameOf(g, attributes));
  if (!e) return kStatusObjectNameNotFound;
  const uint32_t object = e->object;
  const std::string name = e->name;
  const uint32_t handle = AddHandle(object, name);
  if (handle_out) g.Store32(handle_out, handle);
  return kStatusSuccess;
}

uint32_t ObjectManager::ReferenceObjectByName(Guest g, uint32_t name, uint32_t object_out) {
  constexpr uint32_t kStatusObjectNameNotFound = 0xC0000034;
  if (!name) return kStatusInvalidParameter;
  const uint16_t length = g.Load16(name);
  const uint32_t pointer = g.Load32(name + 4);
  const Entry* e = pointer && length ? FindByName(std::string(reinterpret_cast<const char*>(g.host(pointer)), length)) : nullptr;
  if (!e) return kStatusObjectNameNotFound;
  ReferenceObject(e->object);
  if (object_out) g.Store32(object_out, e->object);
  return kStatusSuccess;
}

uint32_t ObjectManager::OpenObjectByPointer(Guest g, uint32_t object, uint32_t handle_out) {
  constexpr uint32_t kStatusUnsuccessful = 0xC0000001;
  std::string name;
  bool known = false;
  for (const Entry& e : entries_) {
    if (e.open && e.object == object) {
      known = true;
      name = e.name;
      break;
    }
  }
  if (!known && !(object >= heap_begin_ && object < heap_next_)) return kStatusUnsuccessful;
  if (!object) return kStatusUnsuccessful;
  const uint32_t handle = AddHandle(object, name);
  if (handle_out) g.Store32(handle_out, handle);
  return kStatusSuccess;
}

uint32_t ObjectManager::SignalAndWait(Guest g, uint32_t signal, uint32_t wait, uint32_t thread, const int64_t* timeout,
                                     bool alertable) {
  constexpr uint32_t kStatusObjectTypeMismatch = 0xC0000024;
  const uint32_t object = Lookup(signal);
  if (!object || !Lookup(wait)) return kStatusInvalidHandle;
  uint32_t status;
  switch (g.Load8(object + kHeaderType)) {
    case kNotificationEvent:
    case kSynchronizationEvent: status = NtSetEvent(g, signal, 0); break;
    case kSemaphore: status = NtReleaseSemaphore(g, signal, 1, 0); break;
    case kMutant: status = NtReleaseMutant(g, signal, thread); break;
    default: return kStatusObjectTypeMismatch;
  }
  if (status != kStatusSuccess) return status;
  return WaitForHandle(g, wait, thread, timeout, alertable);
}

uint32_t ObjectManager::DereferenceObject(Guest, uint32_t object) {
  std::lock_guard lock(lifetime_mutex_);
  auto it = references_.find(object);
  if (it != references_.end() && --it->second <= 0) {
    references_.erase(it);
    Release(object);
  }
  return kStatusSuccess;
}

// --- waits -----------------------------------------------------------------------------------------------------

bool ObjectManager::TrySatisfy(Guest g, uint32_t object, uint32_t thread) {
  const uint8_t type = g.Load8(object + kHeaderType);
  const uint32_t state = g.Load32(object + kHeaderSignalState);
  switch (type) {
    case kNotificationEvent:
      return state != 0;
    case kSynchronizationEvent:
      if (!state) return false;
      g.Store32(object + kHeaderSignalState, 0);
      return true;
    case kSemaphore:
      if (!state) return false;
      g.Store32(object + kHeaderSignalState, state - 1);
      return true;
    case kMutant: {
      const uint32_t owner = g.Load32(object + kMutantOwner);
      if (int32_t(state) <= 0 && owner != thread) return false;
      g.Store32(object + kHeaderSignalState, state - 1);  // recursive acquisition goes negative
      g.Store32(object + kMutantOwner, thread);
      return true;
    }
    case kSynchronizationTimer:
      if (!state) return false;
      g.Store32(object + kHeaderSignalState, 0);
      return true;
    default:
      return state != 0;
  }
}

bool ObjectManager::IsSatisfiable(Guest g, uint32_t object, uint32_t thread) const {
  const uint8_t type = g.Load8(object + kHeaderType);
  const uint32_t state = g.Load32(object + kHeaderSignalState);
  if (type == kMutant) return int32_t(state) > 0 || g.Load32(object + kMutantOwner) == thread;
  return state != 0;
}

uint32_t ObjectManager::WaitForObjects(Guest g, uint32_t count, const uint32_t* objects, uint32_t wait_type, uint32_t thread,
                                       const int64_t* timeout, bool alertable) {
  auto attempt = [&]() -> int32_t {
    Poll(g);
    if (wait_type == kWaitAny) {
      for (uint32_t i = 0; i < count; ++i) {
        if (TrySatisfy(g, objects[i], thread)) return int32_t(i);
      }
      return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
      if (!IsSatisfiable(g, objects[i], thread)) return -1;
    }
    for (uint32_t i = 0; i < count; ++i) TrySatisfy(g, objects[i], thread);
    return 0;
  };
  int32_t taken = attempt();
  if (taken >= 0) return uint32_t(taken);
  const auto limit = WaitLimit(timeout);
  if (limit.count() == 0) return kStatusTimeout;
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    std::this_thread::yield();
    taken = attempt();
    if (taken >= 0) return uint32_t(taken);
    if (alertable && Threads().PendingApcs(thread)) return kStatusUserApc;
    if (timeout && std::chrono::steady_clock::now() - start >= limit) return kStatusTimeout;
  }
}

uint32_t ObjectManager::WaitForHandles(Guest g, uint32_t count, uint32_t handles, uint32_t wait_type, uint32_t thread,
                                       const int64_t* timeout, bool alertable) {
  std::vector<uint32_t> objects(count);
  for (uint32_t i = 0; i < count; ++i) {
    const Entry* e = Find(g.Load32(handles + 4 * i));
    if (!e) return kStatusInvalidParameter;
    objects[i] = e->object;
  }
  for (const uint32_t object : objects) ReferenceObject(object);
  const uint32_t status = WaitForObjects(g, count, objects.data(), wait_type, thread, timeout, alertable);
  for (const uint32_t object : objects) DereferenceObject(g, object);
  return status;
}

uint32_t ObjectManager::WaitForObject(Guest g, uint32_t object, uint32_t thread, const int64_t* timeout, bool alertable) {
  Poll(g);
  if (TrySatisfy(g, object, thread)) return kStatusSuccess;
  const auto limit = WaitLimit(timeout);
  if (limit.count() == 0) return kStatusTimeout;
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    std::this_thread::yield();
    Poll(g);
    if (TrySatisfy(g, object, thread)) return kStatusSuccess;
    if (alertable && Threads().PendingApcs(thread)) return kStatusUserApc;
    if (timeout && std::chrono::steady_clock::now() - start >= limit) return kStatusTimeout;
  }
}

uint32_t ObjectManager::WaitForHandle(Guest g, uint32_t handle, uint32_t thread, const int64_t* timeout, bool alertable) {
  const Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  // The wait keeps the object alive should its handle be closed meanwhile, as the kernel's wait does.
  const uint32_t object = e->object;
  ReferenceObject(object);
  const uint32_t status = WaitForObject(g, object, thread, timeout, alertable);
  DereferenceObject(g, object);
  return status;
}

}  // namespace any360::sys
