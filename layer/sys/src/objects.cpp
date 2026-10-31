#include "any360/sys/objects.h"

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

}  // namespace

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
  heap_next_ = heap_begin_;
}

uint32_t ObjectManager::Allocate(uint32_t size) {
  const uint32_t aligned = (size + 15) & ~15u;
  if (heap_next_ + aligned > heap_end_) return 0;
  const uint32_t at = heap_next_;
  heap_next_ += aligned;
  return at;
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
  const uint32_t object = Allocate(kEventSize);
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
  const uint32_t object = Allocate(kSemaphoreSize);
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
  const uint32_t object = Allocate(kMutantSize);
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

uint32_t ObjectManager::Close(uint32_t handle) {
  Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  e->open = false;
  return kStatusSuccess;
}

uint32_t ObjectManager::Duplicate(Guest g, uint32_t handle, uint32_t new_handle_out, uint32_t options) {
  Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  const uint32_t duplicate = AddHandle(e->object, e->name);
  e = Find(handle);  // the table may have grown
  if (new_handle_out) g.Store32(new_handle_out, duplicate);
  if (options == 1 && e) e->open = false;  // DUPLICATE_CLOSE_SOURCE
  return kStatusSuccess;
}

uint32_t ObjectManager::ReferenceObjectByHandle(Guest g, uint32_t handle, uint32_t thread, uint32_t object_out) {
  if (handle == kCurrentThreadHandle) {
    if (object_out) g.Store32(object_out, thread);
    return kStatusSuccess;
  }
  Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  ++references_[e->object];
  if (object_out) g.Store32(object_out, e->object);
  return kStatusSuccess;
}

uint32_t ObjectManager::DereferenceObject(Guest, uint32_t object) {
  auto it = references_.find(object);
  if (it != references_.end() && --it->second <= 0) references_.erase(it);
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
    default:
      return state != 0;
  }
}

uint32_t ObjectManager::WaitForObject(Guest g, uint32_t object, uint32_t thread, const int64_t* timeout) {
  if (TrySatisfy(g, object, thread)) return kStatusSuccess;
  if (timeout && *timeout == 0) return kStatusTimeout;
  // Relative timeouts are negative 100 ns counts; an absolute one is not supported yet and does not wait.
  if (timeout && *timeout > 0) return kStatusTimeout;
  const auto start = std::chrono::steady_clock::now();
  const auto limit = timeout ? std::chrono::nanoseconds(-*timeout * 100) : std::chrono::nanoseconds::max();
  for (;;) {
    std::this_thread::yield();
    if (TrySatisfy(g, object, thread)) return kStatusSuccess;
    if (timeout && std::chrono::steady_clock::now() - start >= limit) return kStatusTimeout;
  }
}

uint32_t ObjectManager::WaitForHandle(Guest g, uint32_t handle, uint32_t thread, const int64_t* timeout) {
  const Entry* e = Find(handle);
  if (!e) return kStatusInvalidHandle;
  return WaitForObject(g, e->object, thread, timeout);
}

}  // namespace any360::sys
