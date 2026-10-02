// Narrow fallback for Android loader exports consisting of BTI c; B implementation.
// Handles the tail branch or a known PAC/frame prologue, preserving BTI and LR signing.
#pragma once

#include <link.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <unistd.h>

namespace AndroidThunkHook
{
struct TextMapping
{
  uintptr_t address;
  int protection;
};

inline int FindTextMapping(dl_phdr_info *info, size_t, void *opaque)
{
  TextMapping &mapping = *(TextMapping *)opaque;
  bool contains = false;
  for(size_t i = 0; i < info->dlpi_phnum; ++i)
  {
    const ElfW(Phdr) &ph = info->dlpi_phdr[i];
    uintptr_t start = info->dlpi_addr + ph.p_vaddr;
    if(ph.p_type == PT_LOAD && mapping.address >= start &&
       mapping.address + 8 <= start + ph.p_memsz && (ph.p_flags & PF_X))
    {
      mapping.protection = PROT_EXEC | ((ph.p_flags & PF_R) ? PROT_READ : 0) |
                           ((ph.p_flags & PF_W) ? PROT_WRITE : 0);
      contains = true;
    }
  }
  if(!contains)
    return 0;

  // Android's linker applies PROT_BTI for GNU_PROPERTY_AARCH64_FEATURE_1_BTI
  // on hardware advertising HWCAP2_BTI. Preserve it when making text writable.
  if(getauxval(26 /* AT_HWCAP2 */) & (1UL << 17))
  {
    for(size_t i = 0; i < info->dlpi_phnum; ++i)
    {
      const ElfW(Phdr) &ph = info->dlpi_phdr[i];
      if(ph.p_type != PT_NOTE)
        continue;
      const uint8_t *pos = (const uint8_t *)(info->dlpi_addr + ph.p_vaddr);
      const uint8_t *end = pos + ph.p_memsz;
      while(size_t(end - pos) >= sizeof(ElfW(Nhdr)))
      {
        ElfW(Nhdr) note;
        memcpy(&note, pos, sizeof(note));
        pos += sizeof(note);
        size_t nameSize = (size_t(note.n_namesz) + 3) & ~size_t(3);
        size_t descSize = (size_t(note.n_descsz) + 3) & ~size_t(3);
        if(nameSize > size_t(end - pos) || descSize > size_t(end - pos) - nameSize)
          break;
        if(note.n_type == 5 && note.n_namesz == 4 && memcmp(pos, "GNU", 4) == 0)
        {
          const uint8_t *property = pos + nameSize;
          const uint8_t *descEnd = property + note.n_descsz;
          while(size_t(descEnd - property) >= 8)
          {
            uint32_t type, size;
            memcpy(&type, property, 4);
            memcpy(&size, property + 4, 4);
            property += 8;
            size_t padded = (size_t(size) + 7) & ~size_t(7);
            if(padded > size_t(descEnd - property))
              break;
            uint32_t features = 0;
            if(type == 0xc0000000U && size == 4)
              memcpy(&features, property, 4);
            if(features & 1)
              mapping.protection |= 0x10; // PROT_BTI (absent in old NDK headers)
            property += padded;
          }
        }
        pos += nameSize + descSize;
      }
    }
  }
  return 1;
}

inline bool EncodeBranch(uintptr_t from, uintptr_t to, uint32_t &word)
{
  int64_t delta = int64_t(to) - int64_t(from);
  if((delta & 3) || delta < -(1LL << 27) || delta >= (1LL << 27))
    return false;
  word = 0x14000000U | (uint32_t(delta / 4) & 0x03ffffffU);
  return true;
}

// Install during hook registration, before application rendering threads start.
// Successful relay allocations live for the process lifetime, like interceptor-lib hooks.
inline bool Install(void *entry, void *replacement, void **original, bool followImplementation = false)
{
#if defined(__aarch64__)
  if(!entry || !replacement || !original || (uintptr_t(entry) & 3))
    return false;
  uint32_t code[2];
  memcpy(code, entry, sizeof(code));
  if(code[0] != 0xd503245fU || (code[1] & 0xfc000000U) != 0x14000000U)
    return false;

  uintptr_t branch = uintptr_t(entry) + 4;
  int64_t displacement = int64_t(code[1] & 0x03ffffffU);
  if(displacement & (1LL << 25))
    displacement -= 1LL << 26;
  uintptr_t target = uintptr_t(int64_t(branch) + displacement * 4);
  if(target == branch || target == uintptr_t(entry))
    return false;

  bool pacThunk = false;
  uint32_t savedWord = code[1];
  uint32_t pac = 0;
  if(followImplementation)
  {
    TextMapping bodyMapping = {target, 0};
    dl_iterate_phdr(FindTextMapping, &bodyMapping);
    if(bodyMapping.protection)
    {
      uint32_t body[2];
      memcpy(body, (void *)target, sizeof(body));
      // Relocate only a known PC-independent frame allocation. Do not feed PAC
      // through interceptor-lib's generic hint skipping: that signs LR before
      // entering a replacement compiled without pointer authentication.
      bool frameAllocation = (body[1] & 0xffc003ffU) == 0xd10003ffU ||
                             (body[1] & 0xffc003e0U) == 0xa98003e0U;
      if(body[0] == 0xd503233fU && frameAllocation)
      {
        pacThunk = true;
        pac = body[0];
        savedWord = body[1];
        entry = (void *)target;
        branch = target + 4;
        target += 8;
      }
    }
  }

  TextMapping mapping = {uintptr_t(entry), 0};
  dl_iterate_phdr(FindTextMapping, &mapping);
  if(!mapping.protection)
    return false;

  long systemPageSize = sysconf(_SC_PAGESIZE);
  if(systemPageSize <= 0 || (systemPageSize & (systemPageSize - 1)))
    return false;
  size_t pageSize = size_t(systemPageSize);
  uintptr_t page = branch & ~(uintptr_t(pageSize) - 1);
  void *relay = MAP_FAILED;
  uint32_t outward = 0, backward = 0;
  const size_t callbackOffset = pacThunk ? 24 : 16;
  const size_t returnOffset = pacThunk ? 36 : 20;
  // mmap hints never replace mappings. Check both direct branch ranges even if
  // the kernel ignores the hint, and discard out-of-range allocations.
  for(uintptr_t distance = 0x10000; distance < (1U << 27); distance += 0x10000)
  {
    for(int direction = -1; direction <= 1; direction += 2)
    {
      uintptr_t hint = uintptr_t(int64_t(page) + direction * int64_t(distance));
      relay = mmap((void *)hint, pageSize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if(relay == MAP_FAILED)
        continue;
      if(EncodeBranch(branch, uintptr_t(relay), outward) &&
         EncodeBranch(uintptr_t(relay) + returnOffset, target, backward))
        break;
      munmap(relay, pageSize);
      relay = MAP_FAILED;
    }
    if(relay != MAP_FAILED)
      break;
  }
  if(relay == MAP_FAILED)
    return false;

  uint32_t veneer[10] = {0x58000051U, 0xd61f0220U, 0, 0, 0xd503245fU, backward};
  if(pacThunk)
  {
    // Undo the entry's PAC before calling the replacement. The callback signs
    // its own LR and executes the displaced frame allocation exactly once.
    const uint32_t pacVeneer[10] = {0xd50323bfU, 0x58000071U, 0xd61f0220U, 0xd503201fU,
                                    0, 0, 0xd503245fU, pac, savedWord, backward};
    memcpy(veneer, pacVeneer, sizeof(veneer));
    memcpy(veneer + 4, &replacement, sizeof(replacement));
  }
  else
  {
    memcpy(veneer + 2, &replacement, sizeof(replacement));
  }
  memcpy(relay, veneer, sizeof(veneer));
  __builtin___clear_cache((char *)relay, (char *)relay + sizeof(veneer));
  if(mprotect(relay, pageSize, PROT_READ | PROT_EXEC) != 0)
  {
    munmap(relay, pageSize);
    return false;
  }

  if(mprotect((void *)page, pageSize, mapping.protection | PROT_WRITE) != 0)
  {
    munmap(relay, pageSize);
    return false;
  }

  void *previous = *original;
  *original = (char *)relay + callbackOffset;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  __atomic_store_n((uint32_t *)branch, outward, __ATOMIC_RELEASE);
  __builtin___clear_cache((char *)branch, (char *)branch + sizeof(outward));
  if(mprotect((void *)page, pageSize, mapping.protection) != 0)
  {
    __atomic_store_n((uint32_t *)branch, savedWord, __ATOMIC_RELEASE);
    __builtin___clear_cache((char *)branch, (char *)branch + sizeof(outward));
    mprotect((void *)page, pageSize, mapping.protection);
    *original = previous;
    // A caller could already be inside the relay; retain it on this rare failure.
    return false;
  }
  return true;
#else
  return false;
#endif
}
}
