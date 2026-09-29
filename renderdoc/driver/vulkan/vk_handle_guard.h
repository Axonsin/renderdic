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

// [HANDLE-GUARD] Vulkan-only identity tracking. This header deliberately has no Vulkan or
// RenderDoc dependencies so the lifecycle rules can be tested with a counting fake driver.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace VulkanHandleGuard
{
enum class State : uint8_t
{
  Live,
  Claimed,
  Pending,
  Dormant
};
enum class Reason : uint8_t
{
  Valid,
  Unknown,
  Stale,
  WrongType,
  WrongOwner,
  WrongPool,
  NotLive,
  Borrowed,
  Duplicate,
  BadArray,
  LegacyForward,
  Count
};

// These are local registry kinds, never serialised VkResourceType values.
enum SpecialKind : uint32_t
{
  DebugReport = 0x100,
  DebugMessenger,
  PrivateDataSlot,
  DeferredOperation,
  ValidationCache
};

struct Check
{
  uint64_t handle = 0;
  uint32_t type = 0;
  uint64_t owner = 0;
  uint64_t pool = 0;
  bool borrowedAllowed = false;
};

struct Snapshot
{
  void *wrapper = nullptr;
  uint64_t real = 0;
  uint64_t identity = 0;
  uint32_t type = 0;
};

class Registry
{
public:
  explicit Registry(uint32_t generationLimit = UINT32_MAX, uint32_t slotLimit = 1U << 24);
  ~Registry();
  Registry(const Registry &) = delete;
  Registry &operator=(const Registry &) = delete;

  // owner is the identity of the immediate dispatch parent (device or instance), not an address.
  uint64_t Register(void *wrapper, uint32_t type, uint64_t real, uint64_t owner, void *manager,
                    bool dispatchable);
  uint64_t Parent(void *manager, uint32_t type, uint64_t rawOrWrapped);
  uint64_t Identity(uint64_t handle);
  uint64_t PublicHandle(void *wrapper);
  void *Resolve(uint64_t handle, uint32_t type) const;
  bool Inspect(uint64_t handle, Snapshot &snapshot);
  Reason Validate(const Check &check);
  Reason Claim(const Check &check, bool pending = false);
  Reason ClaimBatch(const std::vector<Check> &checks);
  void Cancel(uint64_t handle);
  void Pending(uint64_t handle);
  void Dormant(uint64_t handle);
  uint64_t Renew(void *wrapper, uint64_t real);
  void Retire(void *wrapper);
  void RetireManager(void *manager);
  void RetireOwner(uint64_t owner);
  void SetPool(uint64_t handle, uint64_t pool, bool borrowed = false);
  bool Known(uint64_t handle);
  size_t LiveCount();
  size_t SlotCount();

  static bool IsToken(uint64_t value) { return (value >> 56) == 0xd7; }

private:
  static const uint32_t PageBits = 10;
  static const uint32_t PageSize = 1U << PageBits;
  static const uint32_t PageCount = 1U << (24 - PageBits);
  struct Entry
  {
    std::atomic<void *> wrapper{nullptr};
    std::atomic<uint32_t> generation{0};
    std::atomic<uint32_t> type{0};
    uint64_t real = 0, owner = 0, pool = 0;
    void *manager = nullptr;
    State state = State::Live;
    bool dispatchable = false, borrowed = false;
  };
  using RawKey = std::tuple<void *, uint32_t, uint64_t>;
  Entry *At(uint32_t index) const;
  Entry *Find(uint64_t handle, uint64_t *identity = nullptr);
  Reason ValidateLocked(const Check &check, bool pending = false);
  uint64_t Token(uint32_t index, uint32_t generation) const;
  uint64_t AllocateLocked(void *wrapper, uint32_t type, uint64_t real, uint64_t owner,
                          void *manager, bool dispatchable);
  void RetireLocked(uint64_t identity);
  std::atomic<Entry *> m_Pages[PageCount];
  std::mutex m_Mutex;
  std::unordered_map<void *, uint64_t> m_Wrappers;
  std::unordered_map<uint64_t, uint64_t> m_Dispatchable;
  std::map<RawKey, uint64_t> m_Raw;
  std::vector<uint32_t> m_Free;
  uint32_t m_Size = 0;
  const uint32_t m_GenerationLimit;
  const uint32_t m_SlotLimit;
};

// [HANDLE-GUARD] Keep token resolution inline: every Vulkan resource use passes through here.
// Mutation/ownership checks remain out of line and locked; the normal unwrap path is indexed.
inline Registry::Entry *Registry::At(uint32_t index) const
{
  Entry *page = m_Pages[index >> PageBits].load(std::memory_order_acquire);
  return page ? page + (index & (PageSize - 1)) : nullptr;
}

inline void *Registry::Resolve(uint64_t handle, uint32_t type) const
{
  if(!IsToken(handle))
    return nullptr;
  Entry *e = At(uint32_t(handle & 0xffffff));
  const uint32_t generation = uint32_t(handle >> 24);
  if(!e || e->generation.load(std::memory_order_acquire) != generation || e->type.load() != type)
    return nullptr;
  void *wrapper = e->wrapper.load(std::memory_order_acquire);
  // [HANDLE-GUARD] Do not let a recycled slot turn an old token into a newer wrapper.
  return e->generation.load(std::memory_order_acquire) == generation ? wrapper : nullptr;
}

inline Registry &Get()
{
  // Process lifetime: identities must not restart when an instance is destroyed and recreated.
  // The layer can be used from other static destructors. Avoid static-destruction ordering
  // hazards; this bounded high-water registry is reclaimed by process teardown.
  static Registry *registry = new Registry;
  return *registry;
}
const char *ReasonName(Reason reason);
// Platform policy and diagnostics are outside the registry lock.
bool AllowForeignDestroy();
inline bool MayForward(uint64_t handle, bool enabled, bool known, bool inWrapperArena)
{
  return enabled && !known && !inWrapperArena && !Registry::IsToken(handle) && (handle & 7) == 0 &&
         handle > UINT64_C(0xffffffff);
}
void Report(uint32_t type, Reason reason, uint64_t handle);
void ReportSummary();
}
