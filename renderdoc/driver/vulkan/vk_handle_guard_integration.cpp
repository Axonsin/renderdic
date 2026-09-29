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

// Standalone Android smoke test of the actual layer entrypoints against Vulkan.
// This executable is not linked into RenderDoc. Pass the capture SO as argv[1].
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../../api/app/renderdoc_app.h"
#include "official/vk_layer.h"
#include "official/vulkan.h"

#define CHECK(x)                                                \
  do                                                            \
  {                                                             \
    if(!(x))                                                    \
    {                                                           \
      std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); \
      std::exit(1);                                             \
    }                                                           \
  } while(0)
#define OK(x) CHECK((x) == VK_SUCCESS)

static PFN_vkGetInstanceProcAddr realGPA, layerGPA;
static PFN_vkGetDeviceProcAddr realGDPA, layerGDPA;
static PFN_vkCreateInstance realCreateInstance;
static PFN_vkCreateDevice realCreateDevice;

// The test provides the layer link normally supplied by the Android loader. Remove that link
// before handing the request to the system loader, which creates its own driver chain.
struct StripLinks
{
  struct Node
  {
    VkStructureType type;
    const void *next;
  };
  struct Patch
  {
    const void **address;
    const void *value;
  };
  std::vector<Patch> patches;
  StripLinks(const void *&head)
  {
    const void **previous = &head;
    while(*previous)
    {
      Node *node = (Node *)*previous;
      if(node->type == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
         node->type == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
      {
        patches.push_back({previous, *previous});
        *previous = node->next;
      }
      else
        previous = &node->next;
    }
  }
  ~StripLinks()
  {
    for(auto it = patches.rbegin(); it != patches.rend(); ++it)
      *it->address = it->value;
  }
};

static VkResult VKAPI_CALL NextCreateInstance(const VkInstanceCreateInfo *info,
                                              const VkAllocationCallbacks *allocator,
                                              VkInstance *instance)
{
  VkInstanceCreateInfo copy = *info;
  StripLinks strip(copy.pNext);
  return realCreateInstance(&copy, allocator, instance);
}

static VkResult VKAPI_CALL NextCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                            const VkAllocationCallbacks *allocator, VkDevice *device)
{
  VkDeviceCreateInfo copy = *info;
  StripLinks strip(copy.pNext);
  return realCreateDevice(physical, &copy, allocator, device);
}

static PFN_vkVoidFunction VKAPI_CALL NextGPA(VkInstance instance, const char *name)
{
  if(!std::strcmp(name, "vkCreateInstance"))
    return (PFN_vkVoidFunction)NextCreateInstance;
  if(!std::strcmp(name, "vkCreateDevice"))
    return (PFN_vkVoidFunction)NextCreateDevice;
  return realGPA(instance, name);
}

struct Domain
{
  VkInstance instance = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t family = 0;
  bool debugUtils = false, debugReport = false, privateData = false, deferred = false,
       validationCache = false, bufferAddress = false;
};

static Domain CreateDomain()
{
  Domain domain;
  VkLayerInstanceLink link = {};
  link.pfnNextGetInstanceProcAddr = NextGPA;
  VkLayerInstanceCreateInfo chain = {};
  chain.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
  chain.function = VK_LAYER_LINK_INFO;
  chain.u.pLayerInfo = &link;
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "handle-guard-integration";
  app.apiVersion = VK_API_VERSION_1_2;
  auto instanceExtensions = (PFN_vkEnumerateInstanceExtensionProperties)realGPA(
      VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
  uint32_t extensionCount = 0;
  OK(instanceExtensions(nullptr, &extensionCount, nullptr));
  std::vector<VkExtensionProperties> available(extensionCount);
  OK(instanceExtensions(nullptr, &extensionCount, available.data()));
  std::vector<const char *> enabledInstance;
  for(const auto &ext : available)
  {
    if(!std::strcmp(ext.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
    {
      domain.debugUtils = true;
      enabledInstance.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    if(!std::strcmp(ext.extensionName, VK_EXT_DEBUG_REPORT_EXTENSION_NAME))
    {
      domain.debugReport = true;
      enabledInstance.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
    }
  }
  VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.enabledExtensionCount = uint32_t(enabledInstance.size());
  info.ppEnabledExtensionNames = enabledInstance.data();
  info.pNext = &chain;
  info.pApplicationInfo = &app;
  auto create = (PFN_vkCreateInstance)layerGPA(VK_NULL_HANDLE, "vkCreateInstance");
  CHECK(create);
  OK(create(&info, nullptr, &domain.instance));
  auto enumerate =
      (PFN_vkEnumeratePhysicalDevices)layerGPA(domain.instance, "vkEnumeratePhysicalDevices");
  auto families = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)layerGPA(
      domain.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  uint32_t count = 0;
  OK(enumerate(domain.instance, &count, nullptr));
  CHECK(count);
  std::vector<VkPhysicalDevice> physical(count);
  OK(enumerate(domain.instance, &count, physical.data()));
  families(physical[0], &count, nullptr);
  std::vector<VkQueueFamilyProperties> queues(count);
  families(physical[0], &count, queues.data());
  for(uint32_t i = 0; i < count; ++i)
    if(queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
    {
      domain.family = i;
      break;
    }
  auto deviceExtensions = (PFN_vkEnumerateDeviceExtensionProperties)layerGPA(
      domain.instance, "vkEnumerateDeviceExtensionProperties");
  OK(deviceExtensions(physical[0], nullptr, &extensionCount, nullptr));
  available.resize(extensionCount);
  OK(deviceExtensions(physical[0], nullptr, &extensionCount, available.data()));
  std::vector<const char *> enabledDevice;
  for(const auto &ext : available)
  {
    if(!std::strcmp(ext.extensionName, VK_EXT_PRIVATE_DATA_EXTENSION_NAME))
      domain.privateData = true;
    if(!std::strcmp(ext.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME))
    {
      domain.deferred = true;
      enabledDevice.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    }
    if(!std::strcmp(ext.extensionName, VK_EXT_VALIDATION_CACHE_EXTENSION_NAME))
    {
      domain.validationCache = true;
      enabledDevice.push_back(VK_EXT_VALIDATION_CACHE_EXTENSION_NAME);
    }
  }
  VkPhysicalDevicePrivateDataFeatures privateFeatures = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES};
  if(domain.privateData)
  {
    auto features =
        (PFN_vkGetPhysicalDeviceFeatures2)layerGPA(domain.instance, "vkGetPhysicalDeviceFeatures2");
    VkPhysicalDeviceFeatures2 allFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    allFeatures.pNext = &privateFeatures;
    features(physical[0], &allFeatures);
    domain.privateData = privateFeatures.privateData != VK_FALSE;
    if(domain.privateData)
      enabledDevice.push_back(VK_EXT_PRIVATE_DATA_EXTENSION_NAME);
  }
  VkLayerDeviceLink deviceLink = {};
  VkPhysicalDeviceBufferDeviceAddressFeatures addressFeatures = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
  auto getFeatures =
      (PFN_vkGetPhysicalDeviceFeatures2)layerGPA(domain.instance, "vkGetPhysicalDeviceFeatures2");
  VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.pNext = &addressFeatures;
  getFeatures(physical[0], &features);
  domain.bufferAddress = addressFeatures.bufferDeviceAddress != VK_FALSE;
  addressFeatures.bufferDeviceAddressMultiDevice = VK_FALSE;
  deviceLink.pfnNextGetInstanceProcAddr = NextGPA;
  deviceLink.pfnNextGetDeviceProcAddr = realGDPA;
  VkLayerDeviceCreateInfo deviceChain = {};
  deviceChain.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
  deviceChain.function = VK_LAYER_LINK_INFO;
  deviceChain.u.pLayerInfo = &deviceLink;
  if(domain.privateData)
    deviceChain.pNext = &privateFeatures;
  if(domain.bufferAddress)
  {
    addressFeatures.pNext = const_cast<void *>(deviceChain.pNext);
    deviceChain.pNext = &addressFeatures;
  }
  float priority = 1;
  VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = domain.family;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  deviceInfo.pNext = &deviceChain;
  deviceInfo.enabledExtensionCount = uint32_t(enabledDevice.size());
  deviceInfo.ppEnabledExtensionNames = enabledDevice.data();
  deviceInfo.queueCreateInfoCount = 1;
  deviceInfo.pQueueCreateInfos = &queue;
  auto createDevice = (PFN_vkCreateDevice)layerGPA(domain.instance, "vkCreateDevice");
  OK(createDevice(physical[0], &deviceInfo, nullptr, &domain.device));
  return domain;
}

static VkBool32 VKAPI_CALL DebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                        VkDebugUtilsMessageTypeFlagsEXT,
                                        const VkDebugUtilsMessengerCallbackDataEXT *, void *)
{
  return VK_FALSE;
}
static VkBool32 VKAPI_CALL DebugReport(VkDebugReportFlagsEXT, VkDebugReportObjectTypeEXT, uint64_t,
                                       size_t, int32_t, const char *, const char *, void *)
{
  return VK_FALSE;
}

int main(int argc, char **argv)
{
  CHECK(argc == 2);
  void *vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  CHECK(vulkan);
  realGPA = (PFN_vkGetInstanceProcAddr)dlsym(vulkan, "vkGetInstanceProcAddr");
  realGDPA = (PFN_vkGetDeviceProcAddr)dlsym(vulkan, "vkGetDeviceProcAddr");
  realCreateInstance = (PFN_vkCreateInstance)realGPA(VK_NULL_HANDLE, "vkCreateInstance");
  realCreateDevice = (PFN_vkCreateDevice)dlsym(vulkan, "vkCreateDevice");
  void *layer = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if(!layer)
    std::fprintf(stderr, "%s\n", dlerror());
  CHECK(layer);
  layerGPA =
      (PFN_vkGetInstanceProcAddr)dlsym(layer, "VK_LAYER_RENDERDOC_CaptureGetInstanceProcAddr");
  layerGDPA = (PFN_vkGetDeviceProcAddr)dlsym(layer, "VK_LAYER_RENDERDOC_CaptureGetDeviceProcAddr");
  CHECK(layerGPA && layerGDPA);
  Domain a = CreateDomain();
  Domain b = CreateDomain();
  VkDevice device = a.device;
#define LOAD(name)                                  \
  auto name = (PFN_##name)layerGDPA(device, #name); \
  CHECK(name)
  LOAD(vkCreateBuffer);
  LOAD(vkDestroyBuffer);
  LOAD(vkGetBufferMemoryRequirements);
  LOAD(vkCreateFence);
  LOAD(vkDestroyFence);
  LOAD(vkGetFenceStatus);
  LOAD(vkCreateDescriptorPool);
  LOAD(vkDestroyDescriptorPool);
  LOAD(vkResetDescriptorPool);
  LOAD(vkCreateDescriptorSetLayout);
  LOAD(vkDestroyDescriptorSetLayout);
  LOAD(vkAllocateDescriptorSets);
  LOAD(vkFreeDescriptorSets);
  LOAD(vkCreateCommandPool);
  LOAD(vkDestroyCommandPool);
  LOAD(vkResetCommandPool);
  LOAD(vkAllocateCommandBuffers);
  LOAD(vkFreeCommandBuffers);
  LOAD(vkBeginCommandBuffer);
  LOAD(vkEndCommandBuffer);
  LOAD(vkDestroyDevice);

  VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bufferInfo.size = 4096;
  bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer oldBuffer, buffer;
  OK(vkCreateBuffer(device, &bufferInfo, nullptr, &oldBuffer));
  vkDestroyBuffer(device, oldBuffer, nullptr);
  OK(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer));
  CHECK(oldBuffer != buffer);
  vkDestroyBuffer(device, oldBuffer, nullptr);
  vkDestroyBuffer(device, (VkBuffer)UINT64_C(0xb4000076875cacd8), nullptr);
  vkDestroyBuffer(b.device, buffer, nullptr);
  VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VkFence fence;
  OK(vkCreateFence(device, &fenceInfo, nullptr, &fence));
  vkDestroyBuffer(device, (VkBuffer)fence, nullptr);
  OK(vkGetFenceStatus(device, fence));
  VkMemoryRequirements requirements = {};
  vkGetBufferMemoryRequirements(device, buffer, &requirements);
  CHECK(requirements.size >= bufferInfo.size);
  vkDestroyFence(device, fence, nullptr);
  vkDestroyFence(device, fence, nullptr);
  if(a.privateData)
  {
    auto createSlot = (PFN_vkCreatePrivateDataSlot)layerGDPA(device, "vkCreatePrivateDataSlotEXT");
    auto destroySlot =
        (PFN_vkDestroyPrivateDataSlot)layerGDPA(device, "vkDestroyPrivateDataSlotEXT");
    auto setData = (PFN_vkSetPrivateData)layerGDPA(device, "vkSetPrivateDataEXT");
    auto getData = (PFN_vkGetPrivateData)layerGDPA(device, "vkGetPrivateDataEXT");
    CHECK(createSlot && destroySlot && setData && getData);
    VkPrivateDataSlotCreateInfo slotInfo = {VK_STRUCTURE_TYPE_PRIVATE_DATA_SLOT_CREATE_INFO};
    VkPrivateDataSlot slot;
    OK(createSlot(device, &slotInfo, nullptr, &slot));
    OK(setData(device, VK_OBJECT_TYPE_BUFFER, uint64_t(buffer), slot, 123));
    uint64_t data = 0;
    getData(device, VK_OBJECT_TYPE_BUFFER, uint64_t(buffer), slot, &data);
    CHECK(data == 123);
    destroySlot(b.device, slot, nullptr);
    destroySlot(device, slot, nullptr);
    destroySlot(device, slot, nullptr);
  }
  if(a.debugUtils)
  {
    auto createMessenger =
        (PFN_vkCreateDebugUtilsMessengerEXT)layerGPA(a.instance, "vkCreateDebugUtilsMessengerEXT");
    auto destroyMessenger = (PFN_vkDestroyDebugUtilsMessengerEXT)layerGPA(
        a.instance, "vkDestroyDebugUtilsMessengerEXT");
    LOAD(vkSetDebugUtilsObjectNameEXT);
    VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    messengerInfo.pfnUserCallback = DebugMessage;
    VkDebugUtilsMessengerEXT messenger;
    OK(createMessenger(a.instance, &messengerInfo, nullptr, &messenger));
    VkDebugUtilsObjectNameInfoEXT name = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    name.objectType = VK_OBJECT_TYPE_BUFFER;
    name.objectHandle = uint64_t(buffer);
    name.pObjectName = "guard-buffer";
    OK(vkSetDebugUtilsObjectNameEXT(device, &name));
    destroyMessenger(b.instance, messenger, nullptr);
    destroyMessenger(a.instance, messenger, nullptr);
    destroyMessenger(a.instance, messenger, nullptr);
  }
  if(a.debugReport)
  {
    auto createReport =
        (PFN_vkCreateDebugReportCallbackEXT)layerGPA(a.instance, "vkCreateDebugReportCallbackEXT");
    auto destroyReport = (PFN_vkDestroyDebugReportCallbackEXT)layerGPA(
        a.instance, "vkDestroyDebugReportCallbackEXT");
    VkDebugReportCallbackCreateInfoEXT reportInfo = {
        VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT};
    reportInfo.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT;
    reportInfo.pfnCallback = DebugReport;
    VkDebugReportCallbackEXT report;
    OK(createReport(a.instance, &reportInfo, nullptr, &report));
    destroyReport(b.instance, report, nullptr);
    destroyReport(a.instance, report, nullptr);
    destroyReport(a.instance, report, nullptr);
  }
  if(a.deferred)
  {
    LOAD(vkCreateDeferredOperationKHR);
    LOAD(vkDestroyDeferredOperationKHR);
    LOAD(vkGetDeferredOperationMaxConcurrencyKHR);
    LOAD(vkGetDeferredOperationResultKHR);
    VkDeferredOperationKHR operation;
    OK(vkCreateDeferredOperationKHR(device, nullptr, &operation));
    vkGetDeferredOperationMaxConcurrencyKHR(device, operation);
    vkGetDeferredOperationResultKHR(device, operation);
    vkDestroyDeferredOperationKHR(b.device, operation, nullptr);
    vkDestroyDeferredOperationKHR(device, operation, nullptr);
    vkDestroyDeferredOperationKHR(device, operation, nullptr);
  }
  if(a.validationCache)
  {
    LOAD(vkCreateValidationCacheEXT);
    LOAD(vkDestroyValidationCacheEXT);
    LOAD(vkMergeValidationCachesEXT);
    LOAD(vkGetValidationCacheDataEXT);
    VkValidationCacheCreateInfoEXT cacheInfo = {VK_STRUCTURE_TYPE_VALIDATION_CACHE_CREATE_INFO_EXT};
    VkValidationCacheEXT caches[2];
    OK(vkCreateValidationCacheEXT(device, &cacheInfo, nullptr, &caches[0]));
    OK(vkCreateValidationCacheEXT(device, &cacheInfo, nullptr, &caches[1]));
    OK(vkMergeValidationCachesEXT(device, caches[0], 1, &caches[1]));
    size_t bytes = 0;
    OK(vkGetValidationCacheDataEXT(device, caches[0], &bytes, nullptr));
    vkDestroyValidationCacheEXT(b.device, caches[0], nullptr);
    vkDestroyValidationCacheEXT(device, caches[0], nullptr);
    vkDestroyValidationCacheEXT(device, caches[0], nullptr);
    vkDestroyValidationCacheEXT(device, caches[1], nullptr);
  }
  std::printf(
      "extensions debug_utils=%d debug_report=%d private_data=%d deferred=%d validation_cache=%d\n",
      a.debugUtils, a.debugReport, a.privateData, a.deferred, a.validationCache);
  vkDestroyBuffer(device, buffer, nullptr);

  VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8};
  VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  poolInfo.maxSets = 8;
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes = &poolSize;
  VkDescriptorPool pools[2];
  OK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pools[0]));
  OK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pools[1]));
  VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  VkDescriptorSetLayout layout;
  OK(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout));
  VkDescriptorSetAllocateInfo allocate = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = pools[0];
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &layout;
  VkDescriptorSet set, oldSet;
  OK(vkAllocateDescriptorSets(device, &allocate, &set));
  VkDescriptorSet mixed[] = {set, (VkDescriptorSet)5};
  CHECK(vkFreeDescriptorSets(device, pools[0], 2, mixed) == VK_ERROR_VALIDATION_FAILED_EXT);
  CHECK(vkFreeDescriptorSets(device, pools[1], 1, &set) == VK_ERROR_VALIDATION_FAILED_EXT);
  VkDescriptorSet duplicate[] = {set, set};
  CHECK(vkFreeDescriptorSets(device, pools[0], 2, duplicate) == VK_ERROR_VALIDATION_FAILED_EXT);
  OK(vkFreeDescriptorSets(device, pools[0], 1, &set));
  CHECK(vkFreeDescriptorSets(device, pools[0], 1, &set) == VK_ERROR_VALIDATION_FAILED_EXT);
  OK(vkAllocateDescriptorSets(device, &allocate, &oldSet));
  OK(vkResetDescriptorPool(device, pools[0], 0));
  CHECK(vkFreeDescriptorSets(device, pools[0], 1, &oldSet) == VK_ERROR_VALIDATION_FAILED_EXT);
  OK(vkAllocateDescriptorSets(device, &allocate, &set));
  CHECK(set != oldSet);
  CHECK(vkFreeDescriptorSets(device, pools[0], 1, &oldSet) == VK_ERROR_VALIDATION_FAILED_EXT);
  OK(vkFreeDescriptorSets(device, pools[0], 1, &set));
  vkDestroyDescriptorPool(device, pools[0], nullptr);
  vkDestroyDescriptorPool(device, pools[1], nullptr);
  vkDestroyDescriptorSetLayout(device, layout, nullptr);

  VkCommandPoolCreateInfo commandInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  commandInfo.queueFamilyIndex = a.family;
  VkCommandPool commandPool;
  OK(vkCreateCommandPool(device, &commandInfo, nullptr, &commandPool));
  VkCommandBufferAllocateInfo commandAllocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  commandAllocate.commandPool = commandPool;
  commandAllocate.commandBufferCount = 1;
  commandAllocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  VkCommandBuffer command;
  OK(vkAllocateCommandBuffers(device, &commandAllocate, &command));
  VkCommandBuffer badCommands[] = {command, (VkCommandBuffer)5};
  vkFreeCommandBuffers(device, commandPool, 2, badCommands);
  OK(vkResetCommandPool(device, commandPool, 0));
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  OK(vkBeginCommandBuffer(command, &begin));
  OK(vkEndCommandBuffer(command));
  vkFreeCommandBuffers(device, commandPool, 1, &command);
  vkFreeCommandBuffers(device, commandPool, 1, &command);
  vkDestroyCommandPool(device, commandPool, nullptr);
  // Exercise both trusted drains of pending destruction through the public capture API.
  auto getAPI = (pRENDERDOC_GetAPI)dlsym(layer, "RENDERDOC_GetAPI");
  CHECK(getAPI);
  RENDERDOC_API_1_4_0 *capture = nullptr;
  CHECK(getAPI(eRENDERDOC_API_Version_1_4_0, (void **)&capture));
  capture->SetCaptureFilePathTemplate("/data/local/tmp/rdoc_guard_capture");
  void *captureDevice = RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(a.instance);
  for(unsigned discard = 0; discard < 2; ++discard)
  {
    capture->StartFrameCapture(captureDevice, nullptr);
    CHECK(capture->IsFrameCapturing());
    VkBuffer pending;
    if(a.bufferAddress)
      bufferInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    OK(vkCreateBuffer(device, &bufferInfo, nullptr, &pending));
    vkDestroyBuffer(device, pending, nullptr);
    vkDestroyBuffer(device, pending, nullptr);
    CHECK(discard ? capture->DiscardFrameCapture(captureDevice, nullptr)
                  : capture->EndFrameCapture(captureDevice, nullptr));
    CHECK(!capture->IsFrameCapturing());
    vkDestroyBuffer(device, pending, nullptr);
  }
  std::printf("capture end/discard passed; buffer-device-address=%d\n", a.bufferAddress);
  vkDestroyDevice(device, nullptr);
  vkDestroyDevice(b.device, nullptr);
  auto destroyInstance = (PFN_vkDestroyInstance)layerGPA(a.instance, "vkDestroyInstance");
  destroyInstance(a.instance, nullptr);
  destroyInstance(b.instance, nullptr);
  std::puts("Actual layer integration passed: stale/type/owner/batch/pool/reset/teardown");
}
