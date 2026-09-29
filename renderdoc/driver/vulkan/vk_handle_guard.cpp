/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "vk_handle_guard.h"
#include <new>
#include <unordered_set>

#ifndef VK_HANDLE_GUARD_STANDALONE
#include "common/common.h"
#if ENABLED(RDOC_ANDROID)
#include <sys/system_properties.h>
#endif
#endif

namespace VulkanHandleGuard
{
Registry::Registry(uint32_t generationLimit, uint32_t slotLimit)
    : m_GenerationLimit(generationLimit), m_SlotLimit(slotLimit)
{
  for(auto &page : m_Pages)
    page.store(nullptr, std::memory_order_relaxed);
}

Registry::~Registry()
{
  for(auto &page : m_Pages)
    delete[] page.load(std::memory_order_relaxed);
}

uint64_t Registry::Token(uint32_t index, uint32_t generation) const
{
  return (UINT64_C(0xd7) << 56) | (uint64_t(generation) << 24) | index;
}

Registry::Entry *Registry::Find(uint64_t handle, uint64_t *identity)
{
  if(!IsToken(handle))
  {
    auto it = m_Dispatchable.find(handle);
    if(it == m_Dispatchable.end())
      return nullptr;
    handle = it->second;
  }
  Entry *e = At(uint32_t(handle & 0xffffff));
  if(!e || e->generation.load() != uint32_t(handle >> 24) || !e->wrapper.load())
    return nullptr;
  if(identity)
    *identity = handle;
  return e;
}

uint64_t Registry::AllocateLocked(void *wrapper, uint32_t type, uint64_t real, uint64_t owner,
                                  void *manager, bool dispatchable)
{
  if(!wrapper || m_Wrappers.count(wrapper))
    return 0;
  uint32_t index;
  if(!m_Free.empty())
  {
    index = m_Free.back();
    m_Free.pop_back();
  }
  else
  {
    if(m_Size >= m_SlotLimit || m_Size == (1U << 24))
      return 0;
    index = m_Size;
    if(!At(index))
    {
      Entry *page = new(std::nothrow) Entry[PageSize];
      if(!page)
        return 0;
      m_Pages[index >> PageBits].store(page, std::memory_order_release);
    }
    ++m_Size;
  }
  Entry *e = At(index);
  if(!e)
    return 0;
  uint32_t generation = e->generation.load() + 1;
  uint64_t identity = Token(index, generation);
  e->generation.store(generation);
  e->type.store(type);
  e->real = real;
  e->owner = owner;
  e->pool = 0;
  e->manager = manager;
  e->state = State::Live;
  e->dispatchable = dispatchable;
  e->borrowed = false;
  try
  {
    m_Wrappers[wrapper] = identity;
    if(dispatchable)
      m_Dispatchable[uint64_t(uintptr_t(wrapper))] = identity;
    // Only dispatch parents need reverse lookup by real handle. Keeping all images/descriptors
    // here would add a redundant tree node to every non-dispatchable object.
    if(dispatchable)
      m_Raw[RawKey(manager, type, real)] = identity;
  }
  catch(const std::bad_alloc &)
  {
    // Nothing was published. Leave this slot retired rather than allocating a free-list node
    // while already handling OOM; callers can clean up the newly created driver object.
    m_Wrappers.erase(wrapper);
    if(dispatchable)
      m_Dispatchable.erase(uint64_t(uintptr_t(wrapper)));
    return 0;
  }
  e->wrapper.store(wrapper, std::memory_order_release);
  return dispatchable ? uint64_t(uintptr_t(wrapper)) : identity;
}

uint64_t Registry::Register(void *wrapper, uint32_t type, uint64_t real, uint64_t owner,
                            void *manager, bool dispatchable)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  return AllocateLocked(wrapper, type, real, owner, manager, dispatchable);
}

uint64_t Registry::Parent(void *manager, uint32_t type, uint64_t rawOrWrapped)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  uint64_t identity = 0;
  Entry *e = Find(rawOrWrapped, &identity);
  if(e && e->manager == manager && e->type.load() == type)
    return identity;
  auto it = m_Raw.find(RawKey(manager, type, rawOrWrapped));
  return it == m_Raw.end() ? 0 : it->second;
}

uint64_t Registry::Identity(uint64_t handle)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  uint64_t identity = 0;
  Find(handle, &identity);
  return identity;
}

uint64_t Registry::PublicHandle(void *wrapper)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  auto it = m_Wrappers.find(wrapper);
  if(it == m_Wrappers.end())
    return 0;
  Entry *e = Find(it->second);
  return e->dispatchable ? uint64_t(uintptr_t(wrapper)) : it->second;
}

bool Registry::Inspect(uint64_t handle, Snapshot &snapshot)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  uint64_t identity = 0;
  Entry *e = Find(handle, &identity);
  if(!e)
    return false;
  snapshot.wrapper = e->wrapper.load();
  snapshot.real = e->real;
  snapshot.identity = identity;
  snapshot.type = e->type.load();
  return true;
}

Reason Registry::ValidateLocked(const Check &check, bool pending)
{
  Entry *e = Find(check.handle);
  if(!e)
    return IsToken(check.handle) ? Reason::Stale : Reason::Unknown;
  if(e->type.load() != check.type)
    return Reason::WrongType;
  if(!check.owner || e->owner != check.owner)
    return Reason::WrongOwner;
  if(check.pool && e->pool != check.pool)
    return Reason::WrongPool;
  if(e->borrowed && !check.borrowedAllowed)
    return Reason::Borrowed;
  if(e->state != (pending ? State::Pending : State::Live))
    return Reason::NotLive;
  return Reason::Valid;
}

Reason Registry::Validate(const Check &check)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  return ValidateLocked(check);
}

Reason Registry::Claim(const Check &check, bool pending)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  Reason reason = ValidateLocked(check, pending);
  if(reason == Reason::Valid)
    Find(check.handle)->state = State::Claimed;
  return reason;
}

Reason Registry::ClaimBatch(const std::vector<Check> &checks)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  std::unordered_set<uint64_t> seen;
  for(const Check &check : checks)
  {
    Reason reason = ValidateLocked(check);
    if(reason != Reason::Valid)
      return reason;
    if(!seen.insert(check.handle).second)
      return Reason::Duplicate;
  }
  for(const Check &check : checks)
    Find(check.handle)->state = State::Claimed;
  return Reason::Valid;
}

void Registry::Cancel(uint64_t handle)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  Entry *e = Find(handle);
  if(e && e->state == State::Claimed)
    e->state = State::Live;
}

void Registry::Pending(uint64_t handle)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  Entry *e = Find(handle);
  if(e && e->state == State::Claimed)
    e->state = State::Pending;
}

void Registry::Dormant(uint64_t handle)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  Entry *e = Find(handle);
  if(e)
    e->state = State::Dormant;
}

void Registry::RetireLocked(uint64_t identity)
{
  Entry *e = Find(identity);
  if(!e)
    return;
  void *wrapper = e->wrapper.exchange(nullptr);
  m_Wrappers.erase(wrapper);
  if(e->dispatchable)
    m_Dispatchable.erase(uint64_t(uintptr_t(wrapper)));
  auto raw = m_Raw.find(RawKey(e->manager, e->type.load(), e->real));
  if(raw != m_Raw.end() && raw->second == identity)
    m_Raw.erase(raw);
  // [HANDLE-GUARD] A generation never wraps. Exhausted slots remain permanently retired.
  if(e->generation.load() < m_GenerationLimit)
    m_Free.push_back(uint32_t(identity & 0xffffff));
}

uint64_t Registry::Renew(void *wrapper, uint64_t real)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  auto it = m_Wrappers.find(wrapper);
  if(it == m_Wrappers.end())
    return 0;
  Entry *e = Find(it->second);
  if(e->state != State::Dormant)
    return 0;
  uint32_t type = e->type.load();
  uint64_t owner = e->owner, pool = e->pool;
  void *manager = e->manager;
  bool dispatchable = e->dispatchable, borrowed = e->borrowed;
  RetireLocked(it->second);
  uint64_t result = AllocateLocked(wrapper, type, real, owner, manager, dispatchable);
  if(result)
  {
    e = Find(result);
    e->pool = pool;
    e->borrowed = borrowed;
  }
  return result;
}

void Registry::Retire(void *wrapper)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  auto it = m_Wrappers.find(wrapper);
  if(it != m_Wrappers.end())
    RetireLocked(it->second);
}

void Registry::RetireManager(void *manager)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  for(uint32_t i = 0; i < m_Size; ++i)
  {
    Entry *e = At(i);
    if(e->wrapper.load() && e->manager == manager)
      RetireLocked(Token(i, e->generation.load()));
  }
}

void Registry::RetireOwner(uint64_t owner)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  for(uint32_t i = 0; i < m_Size; ++i)
  {
    Entry *e = At(i);
    if(e->wrapper.load() && e->owner == owner)
    {
      // Pass-through extensions have no resource-manager record to own their small carrier.
      void *carrier = e->wrapper.load();
      const uint32_t type = e->type.load();
      RetireLocked(Token(i, e->generation.load()));
      if(type == PrivateDataSlot || type == DeferredOperation || type == ValidationCache)
        delete(uint64_t *)carrier;
    }
  }
}

void Registry::SetPool(uint64_t handle, uint64_t pool, bool borrowed)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  Entry *e = Find(handle);
  if(e)
  {
    e->pool = pool;
    e->borrowed = borrowed;
  }
}

bool Registry::Known(uint64_t handle)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  return IsToken(handle) || Find(handle) || m_Wrappers.count((void *)(uintptr_t)handle);
}

size_t Registry::LiveCount()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  return m_Wrappers.size();
}

size_t Registry::SlotCount()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  return m_Size;
}

const char *ReasonName(Reason reason)
{
  static const char *names[] = {"valid",       "unknown",    "stale",         "wrong-type",
                                "wrong-owner", "wrong-pool", "not-live",      "borrowed",
                                "duplicate",   "bad-array",  "legacy-forward"};
  return reason < Reason::Count ? names[uint8_t(reason)] : "invalid-reason";
}

#ifndef VK_HANDLE_GUARD_STANDALONE
bool AllowForeignDestroy()
{
  // [HANDLE-GUARD] Read once without spawning getprop. Unknown values fail closed.
  static const bool enabled = []() {
#if ENABLED(RDOC_ANDROID)
    char value[PROP_VALUE_MAX] = {};
    bool allow = __system_property_get("debug.rdoc.foreigndestroy", value) == 1 && value[0] == '1';
    if(allow)
      RDCWARN(
          "[HANDLE-GUARD] Legacy foreign destroy forwarding enabled; unknown values can reach the "
          "driver");
    return allow;
#else
    return false;
#endif
  }();
  return enabled;
}

static std::atomic<uint64_t> rejects[0x105][uint8_t(Reason::Count)];
void Report(uint32_t type, Reason reason, uint64_t handle)
{
  if(type >= 0x105 || reason >= Reason::Count)
    return;
  uint64_t count = ++rejects[type][uint8_t(reason)];
  if(count <= 4)
    RDCWARN("[HANDLE-GUARD] %s type=%u reason=%s handle=%llx count=%llu",
            reason == Reason::LegacyForward ? "forward" : "reject", type, ReasonName(reason),
            (unsigned long long)handle, (unsigned long long)count);
}

void ReportSummary()
{
  RDCLOG("[HANDLE-GUARD] registry entries=%llu slots=%llu", (unsigned long long)Get().LiveCount(),
         (unsigned long long)Get().SlotCount());
  for(uint32_t type = 0; type < 0x105; ++type)
    for(uint8_t reason = 1; reason < uint8_t(Reason::Count); ++reason)
    {
      uint64_t count = rejects[type][reason].load();
      if(count)
        RDCLOG("[HANDLE-GUARD] cumulative type=%u reason=%s count=%llu", type,
               ReasonName(Reason(reason)), (unsigned long long)count);
    }
}
#endif
}
