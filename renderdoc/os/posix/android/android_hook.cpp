/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2026 Baldur Karlsson
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

#include "common/common.h"
#include "common/threading.h"
#include "hooks/hooks.h"
#include "plthook/plthook.h"

#include <android/dlext.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <link.h>
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <set>

// uncomment the following to print (very verbose) debugging prints for the android PLT hooking
// #define HOOK_DEBUG_PRINT(...) RDCLOG(__VA_ARGS__)

#if !defined(HOOK_DEBUG_PRINT)
#define HOOK_DEBUG_PRINT(...) \
  do                          \
  {                           \
  } while(0)
#endif

// from plthook_elf.c
#if defined __x86_64__ || defined __x86_64
#define R_JUMP_SLOT R_X86_64_JUMP_SLOT
#define Elf_Rel ElfW(Rela)
#define ELF_R_TYPE ELF64_R_TYPE
#define ELF_R_SYM ELF64_R_SYM
#elif defined __i386__ || defined __i386
#define R_JUMP_SLOT R_386_JMP_SLOT
#define Elf_Rel ElfW(Rel)
#define ELF_R_TYPE ELF32_R_TYPE
#define ELF_R_SYM ELF32_R_SYM
#elif defined __arm__ || defined __arm
#define R_JUMP_SLOT R_ARM_JUMP_SLOT
#define Elf_Rel ElfW(Rel)
#define ELF_R_TYPE ELF32_R_TYPE
#define ELF_R_SYM ELF32_R_SYM
#elif defined __aarch64__ || defined __aarch64 /* ARM64 */
#define R_JUMP_SLOT R_AARCH64_JUMP_SLOT
#define Elf_Rel ElfW(Rela)
#define ELF_R_TYPE ELF64_R_TYPE
#define ELF_R_SYM ELF64_R_SYM
#else
#error unsupported OS
#endif

class HookingInfo
{
public:
  void AddFunctionHook(const FunctionHook &hook)
  {
    SCOPED_LOCK(lock);
    funchooks.push_back(hook);

    // add to map to speed-up lookup in GetFunctionHook
    funchook_map[hook.function] = hook;
  }

  void AddLibHook(const rdcstr &name)
  {
    SCOPED_LOCK(lock);
    if(!libhooks.contains(name))
      libhooks.push_back(name);
  }

  void AddHookCallback(const rdcstr &name, FunctionLoadCallback callback)
  {
    SCOPED_LOCK(lock);
    hookcallbacks[name].push_back(callback);
  }

  rdcarray<FunctionHook> GetFunctionHooks()
  {
    SCOPED_LOCK(lock);
    return funchooks;
  }

  void ClearHooks()
  {
    SCOPED_LOCK(lock);
    libhooks.clear();
    funchooks.clear();
    funchook_map.clear();
  }

  rdcarray<rdcstr> GetLibHooks()
  {
    SCOPED_LOCK(lock);
    return libhooks;
  }

  std::map<rdcstr, rdcarray<FunctionLoadCallback>> GetHookCallbacks()
  {
    SCOPED_LOCK(lock);
    return hookcallbacks;
  }

  FunctionHook GetFunctionHook(const rdcstr &name)
  {
    SCOPED_LOCK(lock);
    return funchook_map[name];
  }

  bool IsLibHook(const rdcstr &path)
  {
    SCOPED_LOCK(lock);
    for(const rdcstr &filename : libhooks)
    {
      if(path.contains(filename))
      {
        HOOK_DEBUG_PRINT("Intercepting and returning ourselves for %s (matches %s)", path.c_str(),
                         filename.c_str());
        return true;
      }
    }

    return false;
  }

  bool IsLibHook(void *handle)
  {
    SCOPED_LOCK(lock);
    for(const rdcstr &lib : libhooks)
    {
      void *libHandle = dlopen(lib.c_str(), RTLD_NOLOAD);
      HOOK_DEBUG_PRINT("%s is %p", lib.c_str(), libHandle);
      if(libHandle == handle)
        return true;
    }

    return false;
  }

  bool IsHooked(void *handle)
  {
    SCOPED_LOCK(lock);
    bool ret = hooked_handle_already.find(handle) != hooked_handle_already.end();
    return ret;
  }

  bool IsHooked(const rdcstr &soname)
  {
    SCOPED_LOCK(lock);
    if(hooked_soname_already.find(soname) != hooked_soname_already.end())
      return true;

    // above will be absolute path, allow substring matches
    for(const rdcstr &fn : hooked_soname_already)
      if(soname.contains(fn))
        return true;

    return false;
  }

  void SetHooked(void *handle)
  {
    SCOPED_LOCK(lock);
    hooked_handle_already.insert(handle);
  }

  void SetHooked(const rdcstr &soname)
  {
    SCOPED_LOCK(lock);
    hooked_soname_already.insert(soname);
  }

private:
  std::set<rdcstr> hooked_soname_already;
  std::set<void *> hooked_handle_already;

  rdcarray<FunctionHook> funchooks;
  std::map<rdcstr, FunctionHook> funchook_map;
  rdcarray<rdcstr> libhooks;

  std::map<rdcstr, rdcarray<FunctionLoadCallback>> hookcallbacks;

  Threading::CriticalSection lock;
};

HookingInfo &GetHookInfo()
{
  static HookingInfo hookinfo;
  return hookinfo;
}

static bool onlyVulkanLoaderHooks = false;

typedef void *(*pfn__loader_dlopen)(const char *filename, int flags, const void *caller_addr);

typedef void *(*pfnandroid_dlopen_ext)(const char *__filename, int __flags,
                                     const android_dlextinfo *__info);

pfnandroid_dlopen_ext real_android_dlopen_ext = NULL;

pfn__loader_dlopen loader_dlopen = NULL;

void *intercept_dlopen(const char *filename, int flag)
{
  if(filename)
  {
    // if this is a library we're hooking, or a request for our own library in any form, return our
    // own library.
    // We need to intercept requests for our own library, because the android loader makes the
    // completely ridiculous decision to load multiple copies of the same library into a process if
    // it's dlopen'd with different paths. This obviously breaks with our hook install.
    // Vendor driver paths are exempted: the system libEGL dlopen()s the vendor driver by full
    // path, and vendor sonames can be in our library hook list - redirecting that would break
    // driver initialisation completely.
    if(strstr(filename, RENDERDOC_ANDROID_LIBRARY) ||
       (!strstr(filename, "/vendor/") && GetHookInfo().IsLibHook(rdcstr(filename))))
    {
      HOOK_DEBUG_PRINT("Intercepting dlopen for %s", filename);
      // Resolve our own copy in its existing namespace without re-entering a loader hook.
      if(loader_dlopen)
        return loader_dlopen(RENDERDOC_ANDROID_LIBRARY, flag, __builtin_return_address(0));
      return dlopen(RENDERDOC_ANDROID_LIBRARY, flag);
    }
  }

  return NULL;
}

// we need this on both paths since interceptor-lib is unable to hook dlopen in libvulkan.so
static int dl_iterate_callback(struct dl_phdr_info *info, size_t size, void *data)
{
  if(info->dlpi_name == NULL)
  {
    HOOK_DEBUG_PRINT("Skipping NULL entry!");
    return 0;
  }
  rdcstr soname = info->dlpi_name;

  if(onlyVulkanLoaderHooks && soname != "libvulkan.so" && !soname.endsWith("/libvulkan.so"))
    return 0;

  if(GetHookInfo().IsHooked(soname))
    return 0;

  HOOK_DEBUG_PRINT("Hooking %s", soname.c_str());
  GetHookInfo().SetHooked(soname);

  // never rewrite the exporting libraries' own GOT entries. libEGL's internal driver-loading
  // path calls its own eglGetProcAddress while holding its init mutex, and redirecting that
  // self-reference into our wrapper re-enters egl_init_drivers, which deadlocks the thread
  // against itself and freezes the whole process (linker lock is held by our constructor at
  // that point, so every other thread blocks in dlopen too). Application modules are still
  // rewritten here, which is what actually captures frames.
  if(GetHookInfo().IsLibHook(soname))
    return 0;

  for(int ph = 0; ph < info->dlpi_phnum; ph++)
  {
    if(info->dlpi_phdr[ph].p_type != PT_DYNAMIC)
      continue;

    ElfW(Dyn) *dynamic = (ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[ph].p_vaddr);

    ElfW(Sym) *dynsym = NULL;
    const char *strtab = NULL;
    size_t strtabcount = 0;
    Elf_Rel *pltbase = NULL;
    ElfW(Sword) pltcount = 0;

    while(dynamic->d_tag != DT_NULL)
    {
      if(dynamic->d_tag == DT_SYMTAB)
        dynsym = (ElfW(Sym) *)(info->dlpi_addr + dynamic->d_un.d_ptr);
      else if(dynamic->d_tag == DT_STRTAB)
        strtab = (const char *)(info->dlpi_addr + dynamic->d_un.d_ptr);
      else if(dynamic->d_tag == DT_STRSZ)
        strtabcount = dynamic->d_un.d_val;
      else if(dynamic->d_tag == DT_JMPREL)
        pltbase = (Elf_Rel *)(info->dlpi_addr + dynamic->d_un.d_ptr);
      else if(dynamic->d_tag == DT_PLTRELSZ)
        pltcount = dynamic->d_un.d_val / sizeof(Elf_Rel);

      /*
      if(dynamic->d_tag == DT_NEEDED)
        HOOK_DEBUG_PRINT("NEEDED [%i, %s]", dynamic->d_un.d_val, strtab + dynamic->d_un.d_val);
        */

      dynamic++;
    }

    if(!dynsym || !strtab || !pltbase || pltcount == 0 || strtabcount == 0)
    {
      RDCWARN("Missing required section to hook %s", info->dlpi_name);
      continue;
    }

    void **relro_base = NULL;
    void **relro_end = NULL;
    bool relro_failed = false;

    FILE *f = FileIO::fopen(info->dlpi_name, FileIO::ReadText);

    // read the file on disk to get the .relro section
    if(f)
    {
      ElfW(Ehdr) ehdr;
      size_t read = FileIO::fread(&ehdr, sizeof(ehdr), 1, f);

      if(read == 1 && ehdr.e_ident[0] == ELFMAG0 && ehdr.e_ident[1] == 'E' &&
         ehdr.e_ident[2] == 'L' && ehdr.e_ident[3] == 'F')
      {
        FileIO::fseek64(f, ehdr.e_phoff, SEEK_SET);
        for(ElfW(Half) idx = 0; idx < ehdr.e_phnum; idx++)
        {
          ElfW(Phdr) phdr;
          read = FileIO::fread(&phdr, sizeof(phdr), 1, f);
          if(read != 1)
          {
            RDCWARN("Failed reading section");
            break;
          }

          if(phdr.p_type == PT_GNU_RELRO)
          {
            relro_base = (void **)(info->dlpi_addr + phdr.p_vaddr);
            relro_end = (void **)(info->dlpi_addr + phdr.p_vaddr + phdr.p_memsz);
          }
        }
      }
      else
      {
        RDCWARN("Didn't get valid ELF header");
      }

      FileIO::fclose(f);
    }
    else
    {
      RDCWARN("Couldn't open '%s' to look for relro!", info->dlpi_name);
      relro_failed = true;
    }

    if(relro_base)
      HOOK_DEBUG_PRINT("Got relro %p -> %p", relro_base, relro_end);
    HOOK_DEBUG_PRINT("Got %i PLT entries", pltcount);

    int pagesize = sysconf(_SC_PAGE_SIZE);

    for(ElfW(Sword) i = 0; i < pltcount; i++)
    {
      Elf_Rel *plt = pltbase + i;
      if(ELF_R_TYPE(plt->r_info) != R_JUMP_SLOT)
      {
        HOOK_DEBUG_PRINT("[%i]: Mismatched type %i vs %i", i, ELF_R_TYPE(plt->r_info), R_JUMP_SLOT);
        continue;
      }

      size_t idx = ELF_R_SYM(plt->r_info);
      size_t name = dynsym[idx].st_name;
      if(name + 1 > strtabcount)
      {
        HOOK_DEBUG_PRINT("[%i] name out of boundstoo big section header string table index: %zu", i,
                         name);
        continue;
      }

      const char *importname = strtab + name;
      void **import = (void **)(info->dlpi_addr + plt->r_offset);

      HOOK_DEBUG_PRINT("[%i] %s at %p (ptr to %p)", i, importname, import, *import);

      const FunctionHook repl = GetHookInfo().GetFunctionHook(importname);
      if(repl.hook)
      {
        HOOK_DEBUG_PRINT("replacing %s!", importname);

        uintptr_t pagebase = 0;

        if(relro_failed || (relro_base <= import && import <= relro_end))
        {
          if(relro_failed)
            HOOK_DEBUG_PRINT("Couldn't get relro sections - mapping read/write");
          else
            HOOK_DEBUG_PRINT("In relro range - %p <= %p <= %p", relro_base, import, relro_end);
          pagebase = uintptr_t(import) & ~(pagesize - 1);

          int ret = mprotect((void *)pagebase, pagesize, PROT_READ | PROT_WRITE);
          if(ret != 0)
          {
            RDCERR("Couldn't read/write the page: %d %d", ret, errno);
            return 0;
          }

          HOOK_DEBUG_PRINT("Marked page read/write");
        }
        else
        {
          HOOK_DEBUG_PRINT("Not in relro! - %p vs %p vs %p", relro_base, import, relro_end);
        }

        // note we don't save the orig function here, since we want to apply our library priorities
        // and we don't know what order these headers will be iterated in. See EndHookRegistration
        // for where we iterate and fetch all the function pointers we want.
        *import = repl.hook;

        if(pagebase)
        {
          if(relro_failed)
          {
            HOOK_DEBUG_PRINT(
                "Couldn't find relro sections - being conservative and leaving read-write");
          }
          else
          {
            HOOK_DEBUG_PRINT("Moving back to read-only");
            mprotect((void *)pagebase, pagesize, PROT_READ);
          }
        }

        HOOK_DEBUG_PRINT("[%i*] %s at %p (ptr to %p)", i, importname, import, *import);
      }
    }
  }

  return 0;
}

// android has a special dlopen that passes the caller address in; pfn__loader_dlopen and the
// loader_dlopen / real_android_dlopen_ext globals are declared above intercept_dlopen.
uint64_t suppressTLS = 0;

// the swap-family hooks stashed at the end of PatchHookedFunctions, for HookLoadedVendorSwap
// to apply against the vendor driver whenever it maps in.
static rdcarray<FunctionHook> vendorSwapHooks;

// defined below: interceptor builds inline hook the swap family on the vendor driver once
// its image is mapped; non-interceptor builds are a no-op.
static void HookLoadedVendorSwap();

void process_dlopen(const char *filename, int flag)
{
  if(filename && !GetHookInfo().IsHooked(rdcstr(filename)))
  {
    HOOK_DEBUG_PRINT("iterating after %s", filename);
    dl_iterate_phdr(dl_iterate_callback, NULL);
    GetHookInfo().SetHooked(filename);
  }
  else
  {
    HOOK_DEBUG_PRINT("Ignoring");
  }
}

extern "C" __attribute__((visibility("default"))) void *hooked_dlopen(const char *filename, int flag)
{
  // get caller address immediately.
  const void *caller_addr = __builtin_return_address(0);

  HOOK_DEBUG_PRINT("hooked_dlopen for %s | %d", filename, flag);
  void *ret = intercept_dlopen(filename, flag);

  // if we intercepted, return immediately
  if(ret)
    return ret;

  ret = loader_dlopen(filename, flag, caller_addr);
  HOOK_DEBUG_PRINT("Got %p", ret);

  if(filename && ret)
  {
    process_dlopen(filename, flag);

    // vendor drivers load through here after our init - apply the inline swap hooks now
    // that the image exists (see HookLoadedVendorSwap).
    if(strstr(filename, "libEGL_adreno"))
      HookLoadedVendorSwap();
  }

  return ret;
}

extern "C" __attribute__((visibility("default"))) void *hooked_android_dlopen_ext(
    const char *__filename, int __flags, const android_dlextinfo *__info)
{
  HOOK_DEBUG_PRINT("hooked_android_dlopen_ext for %s | %d", __filename, __flags);

  void *ret = intercept_dlopen(__filename, __flags);

  // if we intercepted, return immediately
  if(ret)
    return ret;

  // otherwise return the 'real' result.
  if(real_android_dlopen_ext == NULL)
    ret = real_android_dlopen_ext(__filename, __flags, __info);
  else
    ret = android_dlopen_ext(__filename, __flags, __info);
  HOOK_DEBUG_PRINT("Got %p", ret);

  if(__filename && ret)
  {
    process_dlopen(__filename, __flags);

    if(strstr(__filename, "libEGL_adreno"))
      HookLoadedVendorSwap();
  }

  return ret;
}

bool hooks_suppressed();

extern "C" __attribute__((visibility("default"))) void *hooked_dlsym(void *handle, const char *symbol)
{
  // RTLD_DEFAULT / RTLD_NEXT searches resolve into whichever library actually exports the
  // symbol - commonly libEGL.so for EGL entry points. Applications that look up EGL functions
  // with these special handles instead of a dlopen'd handle would otherwise get unhooked real
  // pointers. Resolve for real, and wrap only when the resolution landed in a hooked library.
  if((handle == RTLD_DEFAULT || handle == RTLD_NEXT) && symbol != NULL && !hooks_suppressed())
  {
    void *ret = dlsym(handle, symbol);
    if(ret == NULL)
      return NULL;

    const FunctionHook repl = GetHookInfo().GetFunctionHook(symbol);
    if(repl.hook)
    {
      Dl_info info = {};
      if(dladdr(ret, &info) != 0 && info.dli_fname != NULL &&
         GetHookInfo().IsLibHook(rdcstr(info.dli_fname)))
        return repl.hook;
    }
    return ret;
  }

  if(handle == NULL || symbol == NULL || hooks_suppressed())
    return dlsym(handle, symbol);

  const FunctionHook repl = GetHookInfo().GetFunctionHook(symbol);

  if(repl.hook == NULL)
    return dlsym(handle, symbol);

  if(!GetHookInfo().IsHooked(handle))
  {
    dl_iterate_phdr(dl_iterate_callback, NULL);
    GetHookInfo().SetHooked(handle);
  }

  HOOK_DEBUG_PRINT("Got dlsym for %s which we want in %p...", symbol, handle);

  if(GetHookInfo().IsLibHook(handle))
  {
    HOOK_DEBUG_PRINT("identified dlsym(%s) we want to interpose! returning %p", symbol, repl.hook);
    return repl.hook;
  }

  void *ret = dlsym(handle, symbol);

  if(ret == NULL)
    return NULL;

  Dl_info info = {};
  dladdr(ret, &info);
  HOOK_DEBUG_PRINT("real ret is %p in %s", ret, info.dli_fname);

  // If the library at this handle genuinely exports the symbol, we want our wrapper instead of
  // the real pointer. Custom EGL/GLES loaders dlopen the vendor driver directly and dlsym()
  // the standard egl*/gl* entry points from it, bypassing the system libEGL.so/libGLESv2.so
  // that our registered library hooks cover. The vendor EGL driver (libEGL_adreno.so) lives in
  // the sphal namespace and is not dlopen-able from here, so check the resolved filename
  // instead. Symbols that don't resolve are still NULL, as they would be without hooking.
  if(info.dli_fname != NULL && strstr(info.dli_fname, "libEGL_adreno") != NULL)
  {
    HOOK_DEBUG_PRINT("identified dlsym(%s) into vendor EGL - returning %p", symbol, repl.hook);
    return repl.hook;
  }

  HOOK_DEBUG_PRINT("identified dlsym(%s) we want to interpose! returning %p", symbol, repl.hook);
  return repl.hook;
}

// dlsym can't reach __loader_dlopen: RTLD_NEXT from our locally-loaded copy doesn't search past
// it, the linker namespace blocks dlopen()ing the linker's own objects, and bionic's
// dl_iterate_phdr doesn't list them either. But every library that wraps dlopen (libdl.so on old
// Android, libdl_android.so on new) already holds the resolved address in its GOT: walk each
// mapped library's JUMP_SLOT relocations for "__loader_dlopen" and read that pointer.
struct LdAndroidLookup
{
  const char *symbol;
  void *result;
};

static int ld_android_lookup_callback(struct dl_phdr_info *info, size_t, void *user)
{
  LdAndroidLookup *data = (LdAndroidLookup *)user;

  if(data->result || info->dlpi_name == NULL ||
     (!rdcstr(info->dlpi_name).endsWith("/libdl.so") &&
      !rdcstr(info->dlpi_name).endsWith("/libdl_android.so")))
    return 0;

  const ElfW(Phdr) *dyn_phdr = NULL;
  for(int ph = 0; ph < info->dlpi_phnum; ph++)
  {
    if(info->dlpi_phdr[ph].p_type == PT_DYNAMIC)
      dyn_phdr = &info->dlpi_phdr[ph];
  }

  if(!dyn_phdr)
    return 0;

  const ElfW(Dyn) *dyn = (const ElfW(Dyn) *)(info->dlpi_addr + dyn_phdr->p_vaddr);

  const ElfW(Sym) *symtab = NULL;
  const char *strtab = NULL;
  const Elf_Rel *jmprel = NULL;
  size_t pltsz = 0;

  for(; dyn->d_tag != DT_NULL; dyn++)
  {
    switch(dyn->d_tag)
    {
      case DT_SYMTAB: symtab = (const ElfW(Sym) *)(info->dlpi_addr + dyn->d_un.d_ptr); break;
      case DT_STRTAB: strtab = (const char *)(info->dlpi_addr + dyn->d_un.d_ptr); break;
      case DT_JMPREL: jmprel = (const Elf_Rel *)(info->dlpi_addr + dyn->d_un.d_ptr); break;
      case DT_PLTRELSZ: pltsz = (size_t)dyn->d_un.d_val; break;
      default: break;
    }
  }

  if(!symtab || !strtab || !jmprel)
    return 0;

  for(size_t i = 0; i < pltsz / sizeof(Elf_Rel); i++)
  {
    const Elf_Rel *rel = jmprel + i;

    if(ELF_R_TYPE(rel->r_info) != R_JUMP_SLOT)
      continue;

    const char *name = strtab + symtab[ELF_R_SYM(rel->r_info)].st_name;

    if(strcmp(name, data->symbol) != 0)
      continue;

    // the linker binds JUMP_SLOTs eagerly at load, so the GOT already holds the real pointer
    data->result = *(void **)(info->dlpi_addr + rel->r_offset);
    RDCLOG("__loader_dlopen via %s GOT %p = %p", info->dlpi_name,
           (void *)(info->dlpi_addr + rel->r_offset), data->result);
    return 1;
  }

  return 0;
}

static void InstallHooksCommon()
{
  // If every API entry was intercepted inline, only the Vulkan loader needs the
  // android_dlopen_ext hook that prevents loading a second copy of this layer.
  // Avoid rewriting application and SDK imports that serve no capture purpose.
  onlyVulkanLoaderHooks = GetHookInfo().GetLibHooks().empty();
  suppressTLS = Threading::AllocateTLSSlot();

  // blacklist hooking certain system libraries or ourselves
  GetHookInfo().SetHooked(RENDERDOC_ANDROID_LIBRARY);
  GetHookInfo().SetHooked("libc.so");
  GetHookInfo().SetHooked("libvndksupport.so");

  real_android_dlopen_ext = &android_dlopen_ext;

  loader_dlopen = (pfn__loader_dlopen)dlsym(RTLD_NEXT, "__loader_dlopen");

  if(!loader_dlopen)
  {
    // RTLD_NEXT from our locally-loaded copy doesn't reach ld-android, and the linker namespace
    // blocks dlopen()ing it directly - read the resolved pointer out of libdl's GOT instead.
    // Used only by intercept_dlopen to reload our own library without re-entering any hook.
    LdAndroidLookup data = {"__loader_dlopen", NULL};
    dl_iterate_phdr(ld_android_lookup_callback, &data);
    loader_dlopen = (pfn__loader_dlopen)data.result;
  }

  if(!loader_dlopen)
  {
    RDCWARN("Couldn't find __loader_dlopen, falling back to slow path for dlopen hooking");
  }

  // Failed inline hooks (and non-interceptor builds) still need dlsym interception
  // so that runtime-resolved pointers receive the wrappers in the fallback list.
  if(!onlyVulkanLoaderHooks)
    LibraryHooks::RegisterFunctionHook("", FunctionHook("dlsym", NULL, (void *)&hooked_dlsym));

  LibraryHooks::RegisterFunctionHook(
      "", FunctionHook("android_dlopen_ext", NULL, (void *)&hooked_android_dlopen_ext));
}

#if defined(RENDERDOC_HAVE_INTERCEPTOR_LIB)

void intercept_error(void *, const char *error_msg)
{
  RDCERR("intercept_error: %s", error_msg);
}

#include "interceptor-lib/include/interceptor.h"
#include "aarch64_thunk_hook.h"

// NTE-class applications bind their swap entry point straight to the vendor driver
// (libEGL_adreno.so, loaded into the sphal namespace - not reachable with dlopen() from our
// namespace). Once the driver image is mapped, locate it through /proc/self/maps, resolve the
// swap family from the on-disk dynsym and inline hook it. EGL.SwapBuffers is pointed at the
// vendor trampoline so the wrapper drives the vendor implementation instead of bouncing back
// through the system libEGL dispatch, which would re-enter the patched vendor entry.
static void HookLoadedVendorSwap()
{
  const char *kPath = "/vendor/lib64/egl/libEGL_adreno.so";

  // find the load bias from the lowest mapping of the file
  uint64_t bias = 0;
  bool mapped = false;
  {
    FILE *maps = FileIO::fopen("/proc/self/maps", FileIO::ReadText);
    if(!maps)
      return;
    char line[512];
    while(fgets(line, sizeof(line), maps))
    {
      if(strstr(line, kPath) == NULL)
        continue;
      unsigned long start = 0, end = 0, off = 0;
      if(sscanf(line, "%lx-%lx %*s %lx", &start, &end, &off) != 3)
        continue;
      if(off == 0 || !mapped)
      {
        bias = start - off;
        mapped = true;
      }
      if(off == 0)
        break;
    }
    FileIO::fclose(maps);
  }

  if(!mapped)
    return;

  RDCLOG("Found loaded %s at bias 0x%llx - hooking vendor swap", kPath,
         (unsigned long long)bias);

  rdcarray<FunctionHook> funchooks;
  if(!vendorSwapHooks.empty())
    funchooks = vendorSwapHooks;
  else
    funchooks = GetHookInfo().GetFunctionHooks();
  const char *want[] = {
      "eglSwapBuffers", "eglSwapBuffersWithDamageKHR", "eglSwapBuffersWithDamageEXT",
      "eglPostSubBufferNV",
  };

  void *intercept = InitializeInterceptor();

  FILE *elf = FileIO::fopen(kPath, FileIO::ReadText);
  if(!elf)
    return;

  ElfW(Ehdr) ehdr;
  FileIO::fseek64(elf, 0, SEEK_SET);
  if(FileIO::fread(&ehdr, sizeof(ehdr), 1, elf) != 1 ||
     memcmp(ehdr.e_ident, ELFMAG, 4) != 0)
  {
    FileIO::fclose(elf);
    return;
  }

  rdcarray<ElfW(Phdr)> phdrs;
  phdrs.resize(ehdr.e_phnum);
  FileIO::fseek64(elf, ehdr.e_phoff, SEEK_SET);
  if(FileIO::fread(&phdrs[0], sizeof(ElfW(Phdr)), ehdr.e_phnum, elf) != (size_t)ehdr.e_phnum)
  {
    FileIO::fclose(elf);
    return;
  }

  auto vaddr_to_file = [&phdrs](ElfW(Addr) v) -> uint64_t {
    for(const ElfW(Phdr) &ph : phdrs)
      if(ph.p_type == PT_LOAD && v >= ph.p_vaddr && v < ph.p_vaddr + ph.p_memsz)
        return ph.p_offset + (v - ph.p_vaddr);
    return v;
  };

  ElfW(Addr) symtab = 0, strtab = 0;
  size_t strsz = 0;
  for(const ElfW(Phdr) &ph : phdrs)
  {
    if(ph.p_type != PT_DYNAMIC)
      continue;
    size_t dyncount = ph.p_filesz / sizeof(ElfW(Dyn));
    rdcarray<ElfW(Dyn)> dyns;
    dyns.resize(dyncount);
    FileIO::fseek64(elf, ph.p_offset, SEEK_SET);
    if(FileIO::fread(&dyns[0], sizeof(ElfW(Dyn)), dyncount, elf) != dyncount)
      break;
    for(const ElfW(Dyn) &d : dyns)
    {
      if(d.d_tag == DT_SYMTAB)
        symtab = d.d_un.d_ptr;
      else if(d.d_tag == DT_STRTAB)
        strtab = d.d_un.d_ptr;
      else if(d.d_tag == DT_STRSZ)
        strsz = d.d_un.d_val;
    }
    break;
  }

  if(!symtab || !strtab || !strsz)
  {
    FileIO::fclose(elf);
    return;
  }

  uint64_t sym_file = vaddr_to_file(symtab);
  uint64_t str_file = vaddr_to_file(strtab);
  size_t symcount = (strtab > symtab) ? (strtab - symtab) / sizeof(ElfW(Sym)) : 0;

  for(size_t i = 0; i < symcount; i++)
  {
    ElfW(Sym) sym;
    FileIO::fseek64(elf, sym_file + i * sizeof(ElfW(Sym)), SEEK_SET);
    if(FileIO::fread(&sym, sizeof(sym), 1, elf) != 1)
      break;
    if(sym.st_value == 0 || sym.st_name == 0 || sym.st_name >= strsz)
      continue;

    char name[128] = {};
    FileIO::fseek64(elf, str_file + sym.st_name, SEEK_SET);
    if(FileIO::fread(name, 1, sizeof(name) - 1, elf) == 0)
      continue;

    for(const char *w : want)
    {
      if(strcmp(name, w) != 0)
        continue;

      for(FunctionHook &h : funchooks)
      {
        if(h.function != w || h.hook == NULL)
          continue;

        void *target = (void *)(uintptr_t)(bias + sym.st_value);
        if(GetHookInfo().IsHooked(target))
          break;

        void *tramp = NULL;
        bool success = InterceptFunction(intercept, target, h.hook, &tramp, &intercept_error);
        if(!success)
        {
          RDCERR("Failed to hook vendor %s at %p", w, target);
          break;
        }

        RDCLOG("Hooked vendor %s at %p", w, target);
        GetHookInfo().SetHooked(target);
        if(h.orig)
          *h.orig = tramp;
        break;
      }
    }
  }

  FileIO::fclose(elf);
}

// exposed for egl_hooks.cpp: called after EGL driver initialisation, when the vendor driver
// is guaranteed to be mapped.
void Android_HookVendorSwap()
{
  HookLoadedVendorSwap();
}

// The module installs this small, read-only profile beside the layer. Reading it
// here makes policy per application and independent of hidden system properties.
static bool UseLegacyVulkanHooks()
{
  Dl_info info = {};
  if(!dladdr((void *)&UseLegacyVulkanHooks, &info) || !info.dli_fname)
    return false;
  rdcstr path = info.dli_fname;
  int slash = path.find_last_of("/");
  if(slash < 0)
    return false;
  path = path.substr(0, slash + 1) + "parasite-compat.conf";
  FILE *file = fopen(path.c_str(), "r");
  if(!file)
    return false;
  char line[128];
  bool legacy = false;
  while(fgets(line, sizeof(line), file))
    if(!strcmp(line, "hook_policy=legacy\n") || !strcmp(line, "hook_policy=legacy"))
      legacy = true;
  fclose(file);
  return legacy;
}

void PatchHookedFunctions()
{
  const bool legacyVulkan = UseLegacyVulkanHooks();
  unsigned shortHooks = 0, inlineHooks = 0, fallbackVulkan = 0;
  RDCLOG("Applying hooks with interceptor-lib");

// see below - Huawei workaround
#if defined(__LP64__)
  LibraryHooks::RegisterLibraryHook("/system/lib64/libhwgl.so", NULL);
#else
  LibraryHooks::RegisterLibraryHook("/system/lib/libhwgl.so", NULL);
#endif

  rdcarray<rdcstr> libs = GetHookInfo().GetLibHooks();
  rdcarray<FunctionHook> funchooks = GetHookInfo().GetFunctionHooks();

  // we just leak this
  void *intercept = InitializeInterceptor();

  std::set<rdcstr> fallbacklibs;
  std::set<FunctionHook> fallbackhooks;

  for(const rdcstr &lib : libs)
  {
    void *handle = dlopen(lib.c_str(), RTLD_NOW);

    bool huawei = lib.contains("libhwgl.so");

    if(!handle)
    {
      HOOK_DEBUG_PRINT("Didn't get handle for %s", lib.c_str());
      continue;
    }

    HOOK_DEBUG_PRINT("Hooking %s = %p", lib.c_str(), handle);

    std::set<void *> foundfunctions;

    for(const FunctionHook &hook : funchooks)
    {
      void *oldfunc = dlsym(handle, hook.function.c_str());

      // UNTESTED workaround taken directly from GAPID, in installer.cpp. Quoted comment:
      /*
            // Huawei implements all functions in this library with prefix,
            // all GL functions in libGLES*.so are just trampolines to his.
            // However, we do not support trampoline interception for now,
            // so try to intercept the internal implementation instead.
      */
      if(huawei && oldfunc == NULL)
        oldfunc = dlsym(handle, ("hw_" + hook.function).c_str());

      if(GetHookInfo().IsHooked(oldfunc))
        continue;

      // these EGL entry points are called re-entrantly by libEGL's own driver-loading code
      // while it holds its driver-init mutex (seen on Adreno/Android 15). Inline-hooking their
      // prologues would redirect that internal call into our wrappers, which call onwards into
      // the real functions and deadlock against the init mutex. Leave them to the PLT fallback
      // below: application GOT entries and dlsym() still intercept them, only libEGL's own
      // internal calls stay stock.
      //
      // The same fallback applies to every non-swap EGL entry point: GOT/dlsym interception
      // already covers application callers for them, and keeping them off the inline-hook
      // path minimises the surface for prologue-relocation surprises on hardened (BTI/PAC)
      // Android 15 libraries. Only the swap family strictly needs inline hooks, because the
      // application can cache those pointers before our GOT rewrite lands.
      bool swapFamily = !strncmp(hook.function.c_str(), "eglSwapBuffers", 14) ||
                        !strncmp(hook.function.c_str(), "eglPostSubBuffer", 16);
      bool adrenoEGL = lib.contains("libEGL_adreno");
      bool vulkan = !strncmp(hook.function.c_str(), "vk", 2);

      // the swap family is inline hooked on the vendor driver only: applications that bypass
      // the system libEGL hit the vendor entry directly, and hooking both would let the
      // system and vendor wrappers recurse into each other endlessly.
      // vulkan hooks must stay inline: UE-class applications load libvulkan dynamically and
      // resolve every entry point with dlsym(), so no GOT rewrite can ever see their calls -
      // the prologue patch on vkGetInstanceProcAddr & co. is the only interception point.
      if(!(vulkan || (swapFamily && adrenoEGL)))
      {
        RDCLOG("Deferring %s to PLT hooking", hook.function.c_str());
        fallbacklibs.insert(lib);
        fallbackhooks.insert(hook);
        continue;
      }

      if(!oldfunc)
      {
        HOOK_DEBUG_PRINT("%s didn't have %s", lib.c_str(), hook.function.c_str());
        continue;
      }

      HOOK_DEBUG_PRINT("Hooking %s::%s = %p with %p", lib.c_str(), hook.function.c_str(), oldfunc,
                       hook.hook);

      void *trampoline = NULL;

      // Android 15 may export only BTI c + a tail branch. Interceptor's absolute
      // jump needs more space than that. Also cover pointers cached to the branch
      // destination by recognising its PAC/frame prologue, without generic relocation.
      bool shortThunk = !legacyVulkan && vulkan && hook.orig &&
                        AndroidThunkHook::Install(oldfunc, hook.hook, hook.orig, true);
      bool success = shortThunk ||
                     InterceptFunction(intercept, oldfunc, hook.hook, &trampoline, &intercept_error);
      if(shortThunk)
        RDCLOG("Hooked Vulkan short thunk %s at %p, callback %p", hook.function.c_str(), oldfunc,
               *hook.orig);

      if(!hook.orig)
        RDCWARN("No original pointer for hook of '%s' - trampoline will be lost!",
                hook.function.c_str());

      if(hook.orig && *hook.orig == NULL)
        *hook.orig = trampoline;

      if(vulkan)
      {
        if(shortThunk) shortHooks++;
        else if(success) inlineHooks++;
        else fallbackVulkan++;
      }
      if(success)
      {
        HOOK_DEBUG_PRINT("Hooked successfully, trampoline is %p", trampoline);
      }
      else
      {
        RDCERR("Failed to hook %s::%s!", lib.c_str(), hook.function.c_str());
        fallbacklibs.insert(lib);
        fallbackhooks.insert(hook);
      }

      GetHookInfo().SetHooked(oldfunc);
    }
  }

  RDCLOG("PARASITE_COMPAT policy=%s vulkan=%s short=%u inline=%u fallback=%u",
         legacyVulkan ? "legacy" : "auto",
         fallbackVulkan ? "fallback" : (shortHooks ? "short" : (inlineHooks ? "inline" : "unknown")),
         shortHooks, inlineHooks, fallbackVulkan);

  // we still need to hook android_dlopen_ext with interceptor-lib so that we can intercept the
  // vulkan loader's attempts to load our library and prevent it from loading a second copy (!!)
  // into the process.
  // Unfortunately, interceptor-lib can't hook this function so we need to set up the PLT hooking.
  // This is just a minimal setup to intercept that one function.
  // the swap-family hooks are stashed first: HookLoadedVendorSwap needs their wrappers when
  // the vendor driver maps in later, but ClearHooks() resets the registry.
  vendorSwapHooks.clear();
  for(const FunctionHook &hook : funchooks)
  {
    if(!strncmp(hook.function.c_str(), "eglSwapBuffers", 14) ||
       !strncmp(hook.function.c_str(), "eglPostSubBuffer", 16))
      vendorSwapHooks.push_back(hook);
  }

  HookLoadedVendorSwap();

  GetHookInfo().ClearHooks();

  for(const rdcstr &l : fallbacklibs)
  {
    RDCLOG("Falling back to PLT hooking for %s", l.c_str());
    GetHookInfo().AddLibHook(l);
  }

  for(const FunctionHook &hook : fallbackhooks)
  {
    RDCLOG("Falling back to PLT hooking for %s", hook.function.c_str());
    GetHookInfo().AddFunctionHook(hook);
  }
}

#else

static void HookLoadedVendorSwap()
{
}

void Android_HookVendorSwap()
{
}

void PatchHookedFunctions()
{
  RDCLOG("Applying hooks with PLT hooks");
}

#endif

bool LibraryHooks::Detect(const char *identifier)
{
  const bool symbol = (dlsym(RTLD_DEFAULT, identifier) != NULL);
  const bool env = (getenv(identifier) != NULL);

  RDCLOG("Detecting symbol %s by dlsym: %s", identifier, symbol ? "yes" : "no");
  RDCLOG("Detecting symbol %s by getenv: %s", identifier, env ? "yes" : "no");

  return symbol || env;
}

void LibraryHooks::RemoveHooks()
{
  RDCERR("Removing hooks is not possible on this platform");
}

void LibraryHooks::ReplayInitialise()
{
  // nothing to do
}

void LibraryHooks::BeginHookRegistration()
{
  // nothing to do
}

void LibraryHooks::RegisterFunctionHook(const char *libraryName, const FunctionHook &hook)
{
  // we don't use the library name on android
  (void)libraryName;
  HOOK_DEBUG_PRINT("Registering function hook for %s: %p", hook.function.c_str(), hook.hook);
  GetHookInfo().AddFunctionHook(hook);
}

void LibraryHooks::RegisterLibraryHook(const char *name, FunctionLoadCallback cb)
{
  GetHookInfo().AddLibHook(name);

  HOOK_DEBUG_PRINT("Registering library hook for %s %s", name, cb ? "with callback" : "");

  // open the library immediately if we can
  dlopen(name, RTLD_NOW);

  if(cb)
    GetHookInfo().AddHookCallback(name, cb);
}

void LibraryHooks::IgnoreLibrary(const char *libraryName)
{
}

void LibraryHooks::EndHookRegistration()
{
  HOOK_DEBUG_PRINT("EndHookRegistration");

  // ensure we load all libraries we can immediately, so they are immediately hooked and don't get
  // loaded later.
  rdcarray<rdcstr> libs = GetHookInfo().GetLibHooks();
  for(const rdcstr &lib : libs)
  {
    void *handle = dlopen(lib.c_str(), RTLD_GLOBAL);
    HOOK_DEBUG_PRINT("%s: %p", lib.c_str(), handle);
  }

  // try to prevent the library from being unloaded, increment our dlopen refcount (might not work
  // on android, but we'll try!)
  // we use RTLD_NOLOAD to prevent a second copy being loaded if this path doesn't refer to
  // ourselves or otherwise breaks because of android's terrible library handling.
  {
    rdcstr selfLib;
    FileIO::GetLibraryFilename(selfLib);
    if(FileIO::exists(selfLib))
    {
      void *handle = dlopen(selfLib.c_str(), RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
      if(handle)
        RDCLOG("Dummy-loaded %s with dlopen to prevent library unload", selfLib.c_str());
      else
        RDCLOG("Failed to dummy-loaded %s with dlopen", selfLib.c_str());
    }
    else
    {
      RDCLOG("Couldn't dummy-load %s because it doesn't exist", selfLib.c_str());
    }
  }

  if(libs.empty())
  {
    RDCLOG("No library hooks registered, not doing any hooking");
    return;
  }

  PatchHookedFunctions();

  // this already hooks dlopen (if possible) and android_dlopen_ext, which is enough
  InstallHooksCommon();

  LibraryHooks::Refresh();

  // iterate our list of libraries and look up the original pointer for any that we don't already
  // have. If we have interceptor-lib this will only be for functions that failed to generate a
  // trampoline and we're PLT hooking - without interceptor-lib this will be all functions, but it
  // will allow us to control the order/priority.
  rdcarray<rdcstr> libraryHooks = GetHookInfo().GetLibHooks();
  rdcarray<FunctionHook> functionHooks = GetHookInfo().GetFunctionHooks();

  RDCLOG("Fetching %zu original function pointers over %zu libraries", functionHooks.size(),
         libraryHooks.size());

  for(auto it = libraryHooks.begin(); it != libraryHooks.end(); ++it)
  {
    void *handle = dlopen(it->c_str(), RTLD_NOLOAD | RTLD_GLOBAL);

    if(handle)
    {
      for(FunctionHook &hook : functionHooks)
      {
        if(hook.orig && *hook.orig == NULL)
          *hook.orig = dlsym(handle, hook.function.c_str());
      }
    }
  }

  RDCLOG("Finished");

  // call the callbacks for any libraries that loaded now. If the library wasn't loaded above then
  // it can't be loaded, since we only hook system libraries.
  std::map<rdcstr, rdcarray<FunctionLoadCallback>> callbacks = GetHookInfo().GetHookCallbacks();
  for(auto it = callbacks.begin(); it != callbacks.end(); ++it)
  {
    void *handle = dlopen(it->first.c_str(), RTLD_GLOBAL);
    if(handle)
    {
      HOOK_DEBUG_PRINT("Calling callbacks for %s", it->first.c_str());
      for(FunctionLoadCallback callback : it->second)
        if(callback)
          callback(handle, it->first.c_str());
    }
  }

  RDCLOG("Called library callbacks - hook registration complete");
}

void LibraryHooks::Refresh()
{
  if(suppressTLS == 0)
  {
    RDCLOG("Not refreshing android hooks with no libraries registered");
    return;
  }

  RDCLOG("Refreshing android hooks...");
  dl_iterate_phdr(dl_iterate_callback, NULL);
  RDCLOG("Refreshed");
}

ScopedSuppressHooking::ScopedSuppressHooking()
{
  if(suppressTLS == 0)
    return;

  uintptr_t old = (uintptr_t)Threading::GetTLSValue(suppressTLS);
  Threading::SetTLSValue(suppressTLS, (void *)(old + 1));
}

ScopedSuppressHooking::~ScopedSuppressHooking()
{
  if(suppressTLS == 0)
    return;

  uintptr_t old = (uintptr_t)Threading::GetTLSValue(suppressTLS);
  Threading::SetTLSValue(suppressTLS, (void *)(old - 1));
}

bool hooks_suppressed()
{
  if(suppressTLS == 0)
    return true;

  return (uintptr_t)Threading::GetTLSValue(suppressTLS) > 0;
}
