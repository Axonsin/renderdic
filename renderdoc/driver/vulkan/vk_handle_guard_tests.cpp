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
#include <thread>
#ifdef VK_HANDLE_GUARD_STANDALONE
#include <cassert>
#include <cstdio>
#define REQUIRE(x) assert(x)
#define GUARD_TEST void RunHandleGuardTests()
#else
#include "common/globalconfig.h"
#if ENABLED(ENABLE_UNIT_TESTS)
#include "catch/catch.hpp"
#define GUARD_TEST TEST_CASE("Vulkan handle lifetime guard", "[vulkan][handle-guard]")
#endif
#endif

#ifdef GUARD_TEST
using namespace VulkanHandleGuard;
GUARD_TEST
{
  Registry registry(3);    // Force generation exhaustion without billions of allocations.
  int device = 0, pool = 0, object = 0, other = 0;
  const uint64_t ownerHandle = registry.Register(&device, 1, 100, 0, &device, true);
  const uint64_t owner = registry.Identity(ownerHandle);
  const uint64_t poolHandle = registry.Register(&pool, 2, 101, owner, &device, false);
  uint64_t handle = registry.Register(&object, 3, 102, owner, &device, false);
  registry.SetPool(handle, registry.Identity(poolHandle));
  Check check;
  check.handle = handle;
  check.type = 3;
  check.owner = owner;
  check.pool = registry.Identity(poolHandle);
  int driverCalls = 0;
  auto destroy = [&](const Check &c) {
    if(registry.Claim(c) == Reason::Valid)
    {
      ++driverCalls;
      Snapshot snapshot;
      REQUIRE(registry.Inspect(c.handle, snapshot));
      registry.Retire(snapshot.wrapper);
    }
  };
  for(uint64_t garbage : {UINT64_C(5), UINT64_C(0xcaf65c66), UINT64_C(0xb4000076875cacd8),
                          UINT64_C(0x511f5e16c0), UINT64_C(0xd7ffffffffffffff)})
  {
    Check bad = check;
    bad.handle = garbage;
    destroy(bad);
  }
  Check bad = check;
  bad.type = 4;
  REQUIRE(registry.Validate(bad) == Reason::WrongType);
  destroy(bad);
  bad = check;
  bad.owner++;
  REQUIRE(registry.Validate(bad) == Reason::WrongOwner);
  destroy(bad);
  bad = check;
  bad.pool++;
  REQUIRE(registry.Validate(bad) == Reason::WrongPool);
  destroy(bad);
  REQUIRE(driverCalls == 0);
  uint64_t random = UINT64_C(0x72438846729183);
  for(unsigned i = 0; i < 10000; ++i)
  {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    bad = check;
    bad.handle = random;
    if(random != handle)
      destroy(bad);
  }
  REQUIRE(driverCalls == 0);
  bad = check;
  bad.pool++;
  REQUIRE(registry.ClaimBatch({check, check}) == Reason::Duplicate);
  REQUIRE(registry.Validate(check) == Reason::Valid);
  REQUIRE(registry.ClaimBatch({check, bad}) == Reason::WrongPool);
  REQUIRE(registry.Validate(check) == Reason::Valid);
  REQUIRE(registry.Claim(check) == Reason::Valid);
  registry.Cancel(handle);    // failed driver free/reset does not invalidate identities
  REQUIRE(registry.Validate(check) == Reason::Valid);
  REQUIRE(registry.Claim(check) == Reason::Valid);
  registry.Pending(handle);
  destroy(check);
  REQUIRE(driverCalls == 0);
  REQUIRE(registry.Resolve(handle, 3) == &object);    // trusted deferred cleanup can still unwrap
  REQUIRE(registry.Claim(check, true) == Reason::Valid);
  REQUIRE(registry.Claim(check, true) == Reason::NotLive);
  registry.Cancel(handle);
  destroy(check);
  destroy(check);
  REQUIRE(driverCalls == 1);
  REQUIRE(registry.Resolve(handle, 3) == nullptr);
  uint64_t replacement = registry.Register(&object, 3, 102, owner, &device, false);
  REQUIRE(replacement != handle);
  destroy(check);
  REQUIRE(driverCalls == 1);
  REQUIRE(registry.Resolve(replacement, 3) == &object);
  registry.Dormant(replacement);
  check.handle = replacement;
  check.pool = 0;
  REQUIRE(registry.Validate(check) == Reason::NotLive);
  replacement = registry.Renew(&object, 103);
  REQUIRE(replacement != check.handle);
  REQUIRE(registry.Resolve(check.handle, 3) == nullptr);
  registry.Dormant(replacement);
  size_t before = registry.SlotCount();
  handle = registry.Renew(&object, 104);    // generation 3 exhausted; must allocate a different slot
  REQUIRE(registry.SlotCount() == before + 1);
  REQUIRE(handle != replacement);
  REQUIRE(registry.Resolve(replacement, 3) == nullptr);
  registry.SetPool(handle, poolHandle, true);
  check.handle = handle;
  REQUIRE(registry.Validate(check) == Reason::Borrowed);
  // A dispatchable object remains a pointer, but unknown/wrong owner pointers are not dereferenced.
  uint64_t command = registry.Register(&other, 5, 105, owner, &device, true);
  REQUIRE(command == uint64_t(uintptr_t(&other)));
  registry.SetPool(command, poolHandle);
  check.handle = command;
  check.type = 5;
  check.pool = poolHandle;
  REQUIRE(registry.ClaimBatch({check}) == Reason::Valid);
  registry.Cancel(command);
  REQUIRE(registry.Validate(check) == Reason::Valid);
  REQUIRE(registry.Parent(&device, 1, 100) == owner);
  REQUIRE(registry.Parent(&other, 1, 100) == 0);
  registry.RetireManager(&device);
  REQUIRE(registry.LiveCount() == 0);
  REQUIRE(registry.Identity(command) == 0);
  REQUIRE(registry.Resolve(handle, 3) == nullptr);

  // The explicit compatibility escape hatch is never reachable for our identities/arenas.
  REQUIRE(!MayForward(UINT64_C(0xb4000076875cacd8), false, false, false));
  REQUIRE(MayForward(UINT64_C(0xb4000076875cacd8), true, false, false));
  REQUIRE(!MayForward(UINT64_C(0xb4000076875cacd8), true, true, false));
  REQUIRE(!MayForward(UINT64_C(0xb4000076875cacd8), true, false, true));
  REQUIRE(!MayForward(UINT64_C(0xcaf65c66), true, false, false));
  REQUIRE(!MayForward(handle, true, false, false));

  Registry bounded(UINT32_MAX, 1);
  uint64_t only = bounded.Register(&object, 3, 1, 7, &device, false);
  REQUIRE(only != 0);
  REQUIRE(bounded.Register(&other, 3, 2, 7, &device, false) == 0);
  REQUIRE(bounded.Resolve(only, 3) == &object);
  REQUIRE(bounded.LiveCount() == 1);

  Registry concurrent;
  Check concurrentCheck;
  concurrentCheck.handle = concurrent.Register(&object, 3, 1, 7, &device, false);
  concurrentCheck.type = 3;
  concurrentCheck.owner = 7;
  std::atomic<unsigned> successfulClaims{0};
  std::vector<std::thread> threads;
  for(unsigned i = 0; i < 8; ++i)
    threads.emplace_back([&]() {
      if(concurrent.Claim(concurrentCheck) == Reason::Valid)
        ++successfulClaims;
    });
  for(auto &thread : threads)
    thread.join();
  REQUIRE(successfulClaims.load() == 1);
  concurrent.Retire(&object);
  // Bounded high-water storage under sustained allocation churn (no per-destroy tombstone map).
  for(unsigned i = 0; i < 100000; ++i)
  {
    uint64_t fresh = concurrent.Register(&object, 3, 1, 7, &device, false);
    REQUIRE(fresh != concurrentCheck.handle);
    REQUIRE(concurrent.Resolve(concurrentCheck.handle, 3) == nullptr);
    REQUIRE(concurrent.Resolve(fresh, 3) == &object);
    concurrent.Retire(&object);
  }
  REQUIRE(concurrent.SlotCount() == 1);
  REQUIRE(concurrent.LiveCount() == 0);

  Registry parents;
  uint64_t parent = parents.Register(&device, 1, 10, 0, &device, true);
  uint64_t oldOwner = parents.Identity(parent);
  uint64_t child = parents.Register(&object, 3, 11, oldOwner, &device, false);
  parents.RetireOwner(oldOwner);
  REQUIRE(parents.Resolve(child, 3) == nullptr);
  parents.Retire(&device);
  parent = parents.Register(&device, 1, 10, 0, &device, true);
  REQUIRE(parents.Identity(parent) != oldOwner);
  child = parents.Register(&object, 3, 11, parents.Identity(parent), &device, false);
  Check oldParent;
  oldParent.handle = child;
  oldParent.type = 3;
  oldParent.owner = oldOwner;
  REQUIRE(parents.Validate(oldParent) == Reason::WrongOwner);
  oldParent.owner = parents.Identity(parent);
  REQUIRE(parents.Validate(oldParent) == Reason::Valid);
  parents.RetireManager(&device);
  REQUIRE(parents.LiveCount() == 0);
}

#ifdef VK_HANDLE_GUARD_STANDALONE
int main()
{
  RunHandleGuardTests();
  std::puts("Vulkan handle guard lifecycle tests passed");
}
#endif
#endif
