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

// Standalone benchmark, deliberately excluded from the layer's build.
// Build with VK_HANDLE_GUARD_STANDALONE together with vk_handle_guard.cpp.
#include "vk_handle_guard.h"
#include <chrono>
#include <cstdio>

using namespace VulkanHandleGuard;

struct FakeResource
{
  uint64_t real;
};

int main()
{
  Registry registry;
  const size_t count = 4096, iterations = 10000000;
  FakeResource objects[count];
  uint64_t handles[count];
  FakeResource *pointers[count];
  for(size_t i = 0; i < count; ++i)
  {
    objects[i].real = i + 1;
    pointers[i] = &objects[i];
    handles[i] = registry.Register(&objects[i], 3, i + 1, 7, &registry, false);
    if(!handles[i])
      return 1;
  }

  for(unsigned run = 0; run < 5; ++run)
  {
    volatile uint64_t checksum = 0;
    auto start = std::chrono::steady_clock::now();
    for(size_t i = 0; i < iterations; ++i)
      checksum += pointers[i % count]->real;
    auto directEnd = std::chrono::steady_clock::now();
    for(size_t i = 0; i < iterations; ++i)
      checksum += ((FakeResource *)registry.Resolve(handles[i % count], 3))->real;
    auto resolveEnd = std::chrono::steady_clock::now();
    double direct = std::chrono::duration<double, std::nano>(directEnd - start).count() / iterations;
    double resolve =
        std::chrono::duration<double, std::nano>(resolveEnd - directEnd).count() / iterations;
    std::printf("run=%u direct_ns=%.3f guarded_ns=%.3f delta_ns=%.3f checksum=%llu\n", run, direct,
                resolve, resolve - direct, (unsigned long long)checksum);
  }

  Registry churn;
  FakeResource resource = {1};
  const size_t cycles = 1000000;
  auto start = std::chrono::steady_clock::now();
  size_t driverCalls = 0;
  for(size_t i = 0; i < cycles; ++i)
  {
    Check check;
    check.handle = churn.Register(&resource, 3, 1, 7, &churn, false);
    check.owner = 7;
    check.type = 3;
    if(churn.Claim(check) != Reason::Valid)
      return 2;
    ++driverCalls;
    churn.Retire(&resource);
  }
  double elapsed =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
  std::printf("create_claim_retire_ns=%.3f cycles=%zu driver_calls=%zu slots=%zu live=%zu\n",
              elapsed / cycles, cycles, driverCalls, churn.SlotCount(), churn.LiveCount());
  return churn.SlotCount() == 1 && churn.LiveCount() == 0 && driverCalls == cycles ? 0 : 3;
}
