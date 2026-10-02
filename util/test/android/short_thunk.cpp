// Standalone Android ARM64 integration test. See README.md in this directory.
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
#include "../../../renderdoc/os/posix/android/aarch64_thunk_hook.h"

extern "C" int backwardThunk(int);
extern "C" int forwardThunk(int);
extern "C" int forwardBody(int);
asm(".text\n.p2align 2\n"
    "backwardBody:\n.inst 0xd503233f\nadd w0,w0,#7\n.inst 0xd50323bf\nret\n"
    ".global backwardThunk\n.type backwardThunk,%function\n"
    "backwardThunk:\n.inst 0xd503245f\nb backwardBody\n"
    ".size backwardThunk, .-backwardThunk\n"
    ".global forwardThunk\n.type forwardThunk,%function\n"
    "forwardThunk:\n.inst 0xd503245f\nb forwardBody\n"
    ".size forwardThunk, .-forwardThunk\n"
    ".global forwardBody\n.type forwardBody,%function\n"
    "forwardBody:\n.inst 0xd503233f\nstp x29,x30,[sp,#-16]!\n"
    "mov x29,sp\nadd w0,w0,#7\nldp x29,x30,[sp],#16\n.inst 0xd50323bf\nret\n"
    ".size forwardBody, .-forwardBody\n");

static int (*backwardOriginal)(int);
static int (*forwardOriginal)(int);
static int BackwardReplacement(int x) { return backwardOriginal(x) * 3; }
static int ForwardReplacement(int x) { return forwardOriginal(x) * 5; }
static PFN_vkGetInstanceProcAddr gipaOriginal;
static PFN_vkCreateInstance createOriginal;
static int lookupCount, createCount;
static PFN_vkVoidFunction GipaReplacement(VkInstance instance, const char *name)
{
  ++lookupCount;
  return gipaOriginal(instance, name);
}
static VkResult CreateReplacement(const VkInstanceCreateInfo *info,
                                  const VkAllocationCallbacks *alloc, VkInstance *instance)
{
  ++createCount;
  return createOriginal(info, alloc, instance);
}

int main()
{
  uint32_t word;
  assert(AndroidThunkHook::EncodeBranch(0x10000000, 0x08000000, word));
  assert(AndroidThunkHook::EncodeBranch(0x10000000, 0x17fffffc, word));
  assert(!AndroidThunkHook::EncodeBranch(0x10000000, 0x18000000, word));
  assert(!AndroidThunkHook::EncodeBranch(0x10000000, 0x07fffffc, word));
  assert(!AndroidThunkHook::EncodeBranch(0x10000000, 0x10000001, word));
  assert(backwardThunk(5) == 12 && forwardThunk(5) == 12);
  assert(AndroidThunkHook::Install((void *)backwardThunk, (void *)BackwardReplacement,
                                   (void **)&backwardOriginal));
  assert(backwardThunk(5) == 36 && backwardOriginal(5) == 12);
  assert(AndroidThunkHook::Install((void *)forwardThunk, (void *)ForwardReplacement,
                                   (void **)&forwardOriginal, true));
  assert(forwardThunk(5) == 60 && forwardOriginal(5) == 12 && forwardBody(5) == 60);
  assert(*(uint32_t *)(void *)backwardThunk == 0xd503245fU);
  assert(*(uint32_t *)(void *)forwardThunk == 0xd503245fU);
  assert(*(uint32_t *)(void *)forwardBody == 0xd503233fU);
  void *unchanged = (void *)forwardOriginal;
  assert(!AndroidThunkHook::Install((void *)ForwardReplacement, (void *)forwardThunk, &unchanged));
  assert(unchanged == (void *)forwardOriginal);

  void *vulkan = dlopen("libvulkan.so", RTLD_NOW);
  assert(vulkan);
  auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(vulkan, "vkGetInstanceProcAddr");
  auto create = (PFN_vkCreateInstance)dlsym(vulkan, "vkCreateInstance");
  assert(gipa && create);
  auto cachedGipa = (PFN_vkGetInstanceProcAddr)gipa(VK_NULL_HANDLE, "vkGetInstanceProcAddr");
  auto cachedCreate = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
  assert(cachedGipa && cachedCreate);
  // Device integration cases require a loader with BTI+B exports and recognised
  // PAC/frame prologues (the Android 15 test device). Unsupported loaders fail here.
  assert(AndroidThunkHook::Install((void *)gipa, (void *)GipaReplacement,
                                   (void **)&gipaOriginal, true));
  assert(AndroidThunkHook::Install((void *)create, (void *)CreateReplacement,
                                   (void **)&createOriginal, true));
  for(int i = 0; i < 10000; ++i)
    assert(gipa(VK_NULL_HANDLE, "vkCreateInstance") == (PFN_vkVoidFunction)cachedCreate);
  assert(cachedGipa(VK_NULL_HANDLE, "vkCreateInstance") == (PFN_vkVoidFunction)cachedCreate);
  assert(lookupCount == 10001);
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "thunk-test", 1,
                            "thunk-test", 1, VK_API_VERSION_1_0};
  VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
  VkInstance instance = VK_NULL_HANDLE;
  assert(cachedCreate(&info, NULL, &instance) == VK_SUCCESS);
  assert(createCount == 1);
  auto destroy = (PFN_vkDestroyInstance)gipaOriginal(instance, "vkDestroyInstance");
  assert(destroy);
  destroy(instance, NULL);
  puts("PASS: branch bounds, forward/backward thunks, PAC callbacks, cached GIPA/CreateInstance, Vulkan lifecycle");
}
