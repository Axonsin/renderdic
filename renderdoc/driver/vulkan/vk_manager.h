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

#pragma once

#include "core/resource_manager.h"
#include "vk_acceleration_structure.h"
#include "vk_resources.h"

class WrappedVulkan;

struct SparseBinding : public VkBindSparseInfo
{
  SparseBinding(WrappedVulkan *vk, VkBuffer unwrappedBuffer,
                const rdcarray<AspectSparseTable> &tables);
  SparseBinding(WrappedVulkan *vk, VkImage unwrappedImage, const rdcarray<AspectSparseTable> &tables);
  SparseBinding(const SparseBinding &) = delete;

  bool invalid = false;

  VkSparseBufferMemoryBindInfo bufBind;
  VkSparseImageMemoryBindInfo imgBind;
  VkSparseImageOpaqueMemoryBindInfo imgOpaqueBind;

  rdcarray<VkSparseMemoryBind> opaqueBinds;
  rdcarray<VkSparseImageMemoryBind> imgBinds;
};

// this struct is copied around and for that reason we explicitly keep it simple and POD. The
// lifetime of the memory allocated is controlled by the resource manager - when preparing or
// serialising, we explicitly set the initial contents, then when the whole system is done with them
// we free them again.
struct VkInitialContents
{
  enum Tag
  {
    BufferCopy = 0,
    ClearColorImage,
    ClearDepthStencilImage,
    DescriptorSet,
    SparseTableOnly,
    PreInit,
  };

  VkInitialContents()
  {
    RDCCOMPILE_ASSERT(std::is_standard_layout<VkInitialContents>::value,
                      "VkInitialContents must be POD");
    memset(this, 0, sizeof(*this));
  }

  VkInitialContents(VkResourceType t, Tag tg)
  {
    memset(this, 0, sizeof(*this));
    type = t;
    tag = tg;
  }

  VkInitialContents(VkResourceType t, MemoryAllocation m)
  {
    memset(this, 0, sizeof(*this));
    type = t;
    mem = m;
  }

  void SnapshotPageTable(const ResourceInfo &resInfo)
  {
    SAFE_DELETE(sparseTables);

    sparseTables = new rdcarray<AspectSparseTable>;
    sparseTables->resize(resInfo.altSparseAspects.size() + 1);

    sparseTables->at(0).aspectMask = resInfo.sparseAspect;
    sparseTables->at(0).table = resInfo.sparseTable;

    for(size_t a = 0; a < resInfo.altSparseAspects.size(); a++)
      sparseTables->at(a + 1) = resInfo.altSparseAspects[a];
  }

  template <typename Configuration>
  void Free(ResourceManager<Configuration> *rm)
  {
    // any of these will be NULL if unused
    SAFE_DELETE_ARRAY(descriptorSlots);
    SAFE_DELETE_ARRAY(descriptorWrites);
    SAFE_DELETE_ARRAY(descriptorInfo);
    SAFE_DELETE_ARRAY(inlineInfo);
    SAFE_DELETE_ARRAY(accelerationStructureWrites);
    SAFE_DELETE_ARRAY(accelerationStructures);
    FreeAlignedBuffer(inlineData);

    rm->ResourceTypeRelease(GetWrapped(buf));

    SAFE_DELETE(sparseTables);
    SAFE_DELETE(sparseBind);

    SAFE_RELEASE(accelerationStructureInfo);

    // MemoryAllocation ise not free'd here
  }

  // for descriptor heaps, when capturing we save the slots, when replaying we store direct writes
  DescriptorSetSlot *descriptorSlots;
  VkWriteDescriptorSet *descriptorWrites;
  VkDescriptorBufferInfo *descriptorInfo;
  VkWriteDescriptorSetInlineUniformBlock *inlineInfo;
  byte *inlineData;
  size_t inlineByteSize;
  VkWriteDescriptorSetAccelerationStructureKHR *accelerationStructureWrites;
  VkAccelerationStructureKHR *accelerationStructures;
  size_t numAccelerationStructures;
  uint32_t numDescriptors;

  // for plain resources, we store the resource type and memory allocation details of the contents
  VkResourceType type;
  VkBuffer buf;
  MemoryAllocation mem;
  Tag tag;

  // for sparse resources. The tables pointer is only valid on capture, it is converted to the queue
  // sparse bind. Similar to the descriptors above
  rdcarray<AspectSparseTable> *sparseTables;
  SparseBinding *sparseBind;

  VkAccelerationStructureInfo *accelerationStructureInfo;
};

struct VulkanResourceManagerConfiguration
{
  typedef WrappedVkRes *WrappedResourceType;
  typedef TypedRealHandle RealResourceType;
  typedef VkResourceRecord RecordType;
  typedef VkInitialContents InitialContentData;
};

struct MemRefInterval
{
  ResourceId memory;
  uint64_t start;
  FrameRefType refType;
};

DECLARE_REFLECTION_STRUCT(MemRefInterval);

class VulkanResourceManager : public ResourceManager<VulkanResourceManagerConfiguration>
{
public:
  VulkanResourceManager(CaptureState &state, WrappedVulkan *core)
      : ResourceManager(state), m_Core(core)
  {
    if(GuardActive())
      VulkanHandleGuard::AllowForeignDestroy();
  }
  void SetState(CaptureState state) { m_State = state; }
  CaptureState GetState() { return m_State; }
  ~VulkanResourceManager()
  {
    if(GuardActive())
    {
      VulkanHandleGuard::ReportSummary();
      VulkanHandleGuard::Get().RetireManager(this);
    }
  }
  void ClearWithoutReleasing()
  {
    // if any objects leaked past, it's no longer safe to delete them as we would
    // be calling Shutdown() after the device that owns them is destroyed. Instead
    // we just have to leak ourselves.
    RDCASSERT(m_InitialContents.empty());
    RDCASSERT(m_ResourceRecords.empty());
    RDCASSERT(m_ResourceMap.empty());
    RDCASSERT(m_WrapperMap.empty());

    m_InitialContents.clear();
    m_ResourceRecords.clear();
    m_ResourceMap.clear();
    m_WrapperMap.clear();
  }

  using ResourceManager::AddResourceRecord;

  template <typename realtype>
  VkResourceRecord *AddResourceRecord(realtype &obj)
  {
    using WrappedType = typename UnwrapHelper<realtype>::Outer;
    WrappedType *wrapped = GetWrapped(obj);
    VkResourceRecord *ret = wrapped->record = ResourceManager::AddResourceRecord(wrapped->id);

    ret->Resource = (WrappedVkRes *)wrapped;
    ret->resType = (VkResourceType)WrappedType::TypeEnum;

    return ret;
  }

  ResourceId GetFirstIDForHandle(uint64_t handle);

  uint32_t DescriptorDataSize(VkDescriptorType type);

  // easy path for getting the wrapped handle cast to the correct type
  template <typename realtype>
  realtype GetHandle(ResourceId id)
  {
    return ToWrappedHandle<realtype>(ResourceManager::GetResource(id));
  }

  // handling memory & image layouts
  template <typename SrcBarrierType>
  void RecordSingleBarrier(rdcarray<rdcpair<ResourceId, ImageRegionState>> &states, ResourceId id,
                           const SrcBarrierType &t, uint32_t nummips, uint32_t numslices);

  void RecordBarriers(rdcarray<rdcpair<ResourceId, ImageRegionState>> &states,
                      const std::map<ResourceId, ImageLayouts> &layouts, uint32_t numBarriers,
                      const VkImageMemoryBarrier *barriers);

  void MergeBarriers(rdcarray<rdcpair<ResourceId, ImageRegionState>> &dststates,
                     rdcarray<rdcpair<ResourceId, ImageRegionState>> &srcstates);

  void ApplyBarriers(uint32_t queueFamilyIndex,
                     rdcarray<rdcpair<ResourceId, ImageRegionState>> &states,
                     std::map<ResourceId, ImageLayouts> &layouts);

  void RecordBarriers(rdcflatmap<ResourceId, ImageState> &states, uint32_t queueFamilyIndex,
                      uint32_t numBarriers, const VkImageMemoryBarrier *barriers);

  // we "downcast" to VkImageMemoryBarrier since we don't care about access bits or pipeline stages,
  // only layouts, and to date the VkImageMemoryBarrier can represent everything in
  // VkImageMemoryBarrier2KHR. This includes new image layouts added (which should only be used if
  // the extension is supported).
  void RecordBarriers(rdcflatmap<ResourceId, ImageState> &states, uint32_t queueFamilyIndex,
                      uint32_t numBarriers, const VkImageMemoryBarrier2 *barriers);

  template <typename SerialiserType>
  void SerialiseImageStates(SerialiserType &ser, std::map<ResourceId, LockingImageState> &states);

  template <typename SerialiserType>
  bool Serialise_DeviceMemoryRefs(SerialiserType &ser, rdcarray<MemRefInterval> &data);

  bool Serialise_ImageRefs(ReadSerialiser &ser, std::map<ResourceId, LockingImageState> &states);

  void InsertDeviceMemoryRefs(WriteSerialiser &ser);

  ResourceId GetID(WrappedVkRes *res)
  {
    if(res == NULL)
      return ResourceId();

    if(IsDispatchableRes(res))
      return ((WrappedVkDispRes *)res)->id;

    return ((WrappedVkNonDispRes *)res)->id;
  }

  template <typename realtype>
  WrappedVkNonDispRes *GetNonDispWrapper(realtype real)
  {
    return (WrappedVkNonDispRes *)GetWrapper(ToTypedHandle(real));
  }

  template <typename realtype>
  WrappedVkDispRes *GetDispWrapper(realtype real)
  {
    return (WrappedVkDispRes *)GetWrapper(ToTypedHandle(real));
  }

  template <typename parenttype, typename realtype>
  ResourceId WrapResource(ResourceId id, parenttype parentObj, realtype &obj,
                          uint64_t allocationPool = 0, bool ownsObject = true)
  {
    RDCASSERT(obj != VK_NULL_HANDLE);

    // on replay, we provide an ID for replayed versions of capture-time resources. For
    // replay-only/internal resources, we auto-gen a resource id.
    // during capture we should always be auto-gen'ing a resource id for obvious reasons.
    if(id == ResourceId())
      id = ResourceIDGen::GetNewUniqueID();
    else
      RDCASSERT(IsReplayMode(m_State));

    typename UnwrapHelper<realtype>::Outer *wrapped =
        new typename UnwrapHelper<realtype>::Outer(obj, id);

    SetTableIfDispatchable(IsCaptureMode(m_State), parentObj, m_Core, wrapped);

    AddResource(id, wrapped);

    if(IsReplayMode(m_State))
      AddWrapper(wrapped, ToTypedHandle(obj));

#if NTE_HANDLE_DIAGNOSTICS && ENABLED(RDOC_ANDROID)
    if(NTETraceType(ToTypedHandle(obj).type))
      RDCLOG("NTECREATE: type=%d manager=%p parent=%p obj=%p real=%p id=%s",
             (int)ToTypedHandle(obj).type, this, (void *)parentObj, wrapped, (void *)obj,
             ToStr(id).c_str());
#endif
    // [HANDLE-GUARD] Register before exposing any handle to the application. Raw dispatch
    // parents are resolved within this manager, never by globally comparing driver addresses.
    if(GuardActive())
    {
      auto &guard = VulkanHandleGuard::Get();
      uint64_t owner =
          guard.Parent(this, UnwrapHelper<parenttype>::Outer::TypeEnum, uint64_t(parentObj));
      uint64_t handle = guard.Register(wrapped, UnwrapHelper<realtype>::Outer::TypeEnum,
                                       uint64_t(obj), owner, this, IsDispatchable(obj));
      if(!handle)
      {
        // [HANDLE-GUARD] Never publish a raw handle on identity exhaustion. Like RenderDoc's
        // wrapper allocator, an unrecoverable host allocation failure terminates capture/process.
        // Release the new driver object first, using its actual parent (not m_Device).
        GuardRegistrationFailed(UnwrapHelper<realtype>::Outer::TypeEnum, uint64_t(obj),
                                UnwrapHelper<parenttype>::Outer::TypeEnum, uint64_t(parentObj),
                                allocationPool, ownsObject);
        ResourceManager::ReleaseResource(id);
        delete wrapped;
        obj = VK_NULL_HANDLE;
        RDCFATAL("[HANDLE-GUARD] Cannot register Vulkan resource identity");
      }
      obj = realtype(handle);
    }
    else
      obj = realtype((uint64_t)wrapped);

    return id;
  }

  template <typename realtype>
  ResourceId WrapReusedResource(VkResourceRecord *record, realtype &obj)
  {
    RDCASSERT(obj != VK_NULL_HANDLE);

    typename UnwrapHelper<realtype>::Outer *wrapped =
        (typename UnwrapHelper<realtype>::Outer *)record->Resource;
    wrapped->real = ToTypedHandle(obj).real;

    // [HANDLE-GUARD] A retained descriptor record is not a retained application identity.
    if(GuardActive())
    {
      uint64_t handle = VulkanHandleGuard::Get().Renew(wrapped, uint64_t(obj));
      if(!handle)
        RDCFATAL("[HANDLE-GUARD] Cannot renew descriptor identity");
      obj = realtype(handle);
    }
    else
      obj = realtype((uint64_t)wrapped);

    return wrapped->id;
  }

  template <typename realtype>
  realtype CreateDeferredHandle()
  {
    // only defer non dispatchable handles
    RDCCOMPILE_ASSERT(UnwrapHelper<realtype>::DispatchableType == 0,
                      "Can't defer dispatchable handle");

    realtype ret = (realtype)(m_DummyHandle);

    Atomic::Dec64((int64_t *)&m_DummyHandle);

    return ret;
  }

  void ResolveDeferredWrappers()
  {
    rdcarray<rdcpair<TypedRealHandle, WrappedVkRes *>> wrappers;
    for(auto it = m_WrapperMap.begin(); it != m_WrapperMap.end();)
    {
      if(it->first.real.handle >= m_DummyHandle)
      {
        wrappers.push_back({it->first, it->second});
        it = m_WrapperMap.erase(it);
        continue;
      }

      ++it;
    }

    for(rdcpair<TypedRealHandle, WrappedVkRes *> &wrapper : wrappers)
    {
      // we can know for sure that these are non-dispatchable based on the assert above, to get the new real handle
      WrappedVkNonDispRes *wrapped = (WrappedVkNonDispRes *)wrapper.second;
      wrapper.first.real = wrapped->real;
      wrapped->deferredJob = NULL;
      AddWrapper(wrapper.second, wrapper.first);
    }
  }

  void PreFreeMemory(ResourceId id)
  {
    if(IsActiveCapturing(m_State))
    {
      ResourceManager::Begin_PrepareInitialBatch();
      ResourceManager::Prepare_InitialStateIfPostponed(id, true);
      ResourceManager::End_PrepareInitialBatch();
    }
  }

  void GuardRegistrationFailed(uint32_t type, uint64_t real, uint32_t parentType, uint64_t parent,
                               uint64_t allocationPool, bool ownsObject);

  bool GuardActive() const { return ENABLED(RDOC_ANDROID) && IsCaptureMode(m_State); }

  uint64_t RegisterSpecial(uint64_t parent, uint32_t type, uint64_t real, void *wrapper)
  {
    if(!GuardActive())
      return real;
    return VulkanHandleGuard::Get().Register(
        wrapper, type, real, VulkanHandleGuard::Get().Identity(parent), this, false);
  }

  bool SpecialHandle(uint64_t parent, uint32_t type, uint64_t handle,
                     VulkanHandleGuard::Snapshot &snapshot, bool destroy = false)
  {
    if(!GuardActive())
    {
      snapshot.real = handle;
      snapshot.wrapper = (void *)(uintptr_t)handle;
      return true;
    }
    VulkanHandleGuard::Check check;
    check.handle = handle;
    check.type = type;
    check.owner = VulkanHandleGuard::Get().Identity(parent);
    auto reason =
        destroy ? VulkanHandleGuard::Get().Claim(check) : VulkanHandleGuard::Get().Validate(check);
    if(reason != VulkanHandleGuard::Reason::Valid)
    {
      VulkanHandleGuard::Report(type, reason, handle);
      return false;
    }
    return VulkanHandleGuard::Get().Inspect(handle, snapshot);
  }

  template <typename Parent, typename Object>
  VulkanHandleGuard::Check GuardCheck(Parent parent, Object object, uint64_t pool = 0)
  {
    VulkanHandleGuard::Check check;
    check.handle = uint64_t(object);
    check.type = UnwrapHelper<Object>::Outer::TypeEnum;
    check.owner = VulkanHandleGuard::Get().Identity(uint64_t(parent));
    check.pool = pool;
    return check;
  }

  template <typename Parent, typename Object>
  bool BeginDestroy(Parent parent, Object object, bool deferred = false)
  {
    if(!GuardActive())
      return UnwrapHelper<Object>::Outer::IsMember((const void *)(uintptr_t)object);
    auto check = GuardCheck(parent, object);
    auto reason = VulkanHandleGuard::Get().Claim(check, deferred);
    if(reason != VulkanHandleGuard::Reason::Valid)
      VulkanHandleGuard::Report(
          check.type, CanForwardForeign(object) ? VulkanHandleGuard::Reason::LegacyForward : reason,
          check.handle);
    return reason == VulkanHandleGuard::Reason::Valid;
  }

  template <typename Object>
  bool CanForwardForeign(Object object)
  {
    if(!GuardActive())
      return IsPlausibleDriverHandle((const void *)(uintptr_t)object);
    // [HANDLE-GUARD] Compatibility never forwards a token, a live wrapper, or any address
    // in our wrapper arenas (including freed slots and interior pointers).
    return VulkanHandleGuard::MayForward(uint64_t(object), VulkanHandleGuard::AllowForeignDestroy(),
                                         VulkanHandleGuard::Get().Known(uint64_t(object)),
                                         IsWrappedHandle((WrappedVkRes *)(uintptr_t)object));
  }

  template <typename Parent, typename Object>
  bool ValidateHandle(Parent parent, Object object, bool claim = false)
  {
    if(!GuardActive())
      return true;
    auto check = GuardCheck(parent, object);
    auto reason =
        claim ? VulkanHandleGuard::Get().Claim(check) : VulkanHandleGuard::Get().Validate(check);
    if(reason != VulkanHandleGuard::Reason::Valid)
      VulkanHandleGuard::Report(check.type, reason, check.handle);
    return reason == VulkanHandleGuard::Reason::Valid;
  }

  template <typename Parent, typename Pool, typename Object>
  bool BeginFreeBatch(Parent parent, Pool pool, uint32_t count, const Object *objects)
  {
    if(!GuardActive())
      return true;
    if(!ValidateHandle(parent, pool))
      return false;
    if(count && !objects)
    {
      VulkanHandleGuard::Report(UnwrapHelper<Object>::Outer::TypeEnum,
                                VulkanHandleGuard::Reason::BadArray, 0);
      return false;
    }
    std::vector<VulkanHandleGuard::Check> checks;
    checks.reserve(count);
    uint64_t poolId = VulkanHandleGuard::Get().Identity(uint64_t(pool));
    for(uint32_t i = 0; i < count; ++i)
      if(objects[i] != VK_NULL_HANDLE)
        checks.push_back(GuardCheck(parent, objects[i], poolId));
    // [HANDLE-GUARD] Validate the entire batch before claiming any member. No partial frees.
    auto reason = VulkanHandleGuard::Get().ClaimBatch(checks);
    if(reason != VulkanHandleGuard::Reason::Valid)
      VulkanHandleGuard::Report(UnwrapHelper<Object>::Outer::TypeEnum, reason, uint64_t(pool));
    return reason == VulkanHandleGuard::Reason::Valid;
  }

  template <typename Object, typename Pool>
  void GuardSetPool(Object object, Pool pool, bool borrowed = false)
  {
    if(GuardActive())
      VulkanHandleGuard::Get().SetPool(uint64_t(object),
                                       VulkanHandleGuard::Get().Identity(uint64_t(pool)), borrowed);
  }

  void RemoveAnnotations(ResourceId id);

  template <typename realtype>
  void ReleaseWrappedResource(realtype obj, bool clearID = false)
  {
    // [HANDLE-GUARD] This is trusted internal cleanup, also used for claimed/pending/dormant
    // objects. Public API boundaries validate ownership/liveness before reaching here. Token
    // decoding rejects retired generations without touching a reused wrapper slot.
    void *wrapped = (void *)GetWrapped(obj);
    if(obj != VK_NULL_HANDLE && wrapped == NULL)
      return;
    if(wrapped != NULL && !IsWrappedHandleStrict((WrappedVkRes *)wrapped))
    {
      RDCERR("ReleaseWrappedResource: %p (wrapped %p) is not one of our wrapped objects - skipping",
             (void *)obj, wrapped);
      return;
    }

    ResourceId id = GetResID(obj);
#if NTE_HANDLE_DIAGNOSTICS && ENABLED(RDOC_ANDROID)
    if(obj != VK_NULL_HANDLE && NTETraceType(ToTypedHandle(obj).type))
      RDCLOG("NTERELEASE: type=%d manager=%p obj=%p real=%p id=%s",
             (int)ToTypedHandle(obj).type, this, (void *)obj, (void *)Unwrap(obj),
             ToStr(id).c_str());
#endif

    RemoveAnnotations(id);

    if(IsReplayMode(m_State))
    {
      ResourceManager::RemoveWrapper(GetWrapped(obj), ToTypedHandle(Unwrap(obj)));
    }

    ResourceManager::ReleaseResource(id);
    VkResourceRecord *record = GetRecord(obj);
    if(record)
    {
      // we need to lock here because the app could be creating
      // and deleting from this pool at the same time. We do know
      // though that the pool isn't going to be destroyed while
      // either allocation or freeing happens, so we only need to
      // lock against concurrent allocs or deletes of children.

      if(ToTypedHandle(obj).type == eResCommandBuffer && record->cmdInfo &&
         record->cmdInfo->allocRecord)
      {
        record->cmdInfo->allocRecord->Delete(this);
        record->cmdInfo->allocRecord = NULL;
      }

      if(record->bakedCommands)
      {
        record->bakedCommands->Delete(this);
        record->bakedCommands = NULL;
      }

      if(record->pool)
      {
        // here we lock against concurrent alloc/delete and remove it from our pool so we don't try
        // and destroy it
        record->pool->LockChunks();
        record->pool->pooledChildren.removeOne(record);
        record->pool->UnlockChunks();
      }
      else if(record->pooledChildren.size())
      {
        // delete all of our children
        for(auto it = record->pooledChildren.begin(); it != record->pooledChildren.end(); ++it)
        {
          // unset record->pool so we don't recurse
          (*it)->pool = NULL;
          VkResourceType restype = IdentifyTypeByPtr((*it)->Resource);
          if(restype == eResDescriptorSet)
            ReleaseWrappedResource(ToWrappedHandle<VkDescriptorSet>((*it)->Resource), true);
          else if(restype == eResCommandBuffer)
            ReleaseWrappedResource((VkCommandBuffer)(*it)->Resource, true);
          else if(restype == eResQueue)
            ReleaseWrappedResource((VkQueue)(*it)->Resource, true);
          else if(restype == eResPhysicalDevice)
            ReleaseWrappedResource((VkPhysicalDevice)(*it)->Resource, true);
          else
            RDCERR("Unexpected resource type %d as pooled child!", restype);
        }
        record->pooledChildren.clear();
      }

      record->Delete(this);
    }
    if(clearID)
    {
      // note the nulling of the wrapped object's ID here is rather unpleasant,
      // but the lesser of two evils to ensure that stale descriptor set slots
      // referencing the object behave safely. To do this correctly we would need
      // to maintain a list of back-references to every descriptor set that has
      // this object bound, and invalidate them. Instead we just make sure the ID
      // is always something sensible, since we know the deallocation doesn't
      // free the memory - the object is pool-allocated.
      // If a new object is allocated in that pool slot, it will still be a valid
      // ID and if the resource isn't ever referenced elsewhere, it will just be
      // a non-live ID to be ignored.

      if(IsDispatchable(obj))
      {
        WrappedVkDispRes *res = (WrappedVkDispRes *)GetWrapped(obj);
        res->id = ResourceId();
        res->record = NULL;
      }
      else
      {
        WrappedVkNonDispRes *res = (WrappedVkNonDispRes *)GetWrapped(obj);
        res->id = ResourceId();
        res->record = NULL;
      }
    }
    auto *released = GetWrapped(obj);
    if(GuardActive())
    {
      if(ToTypedHandle(obj).type == eResDevice || ToTypedHandle(obj).type == eResInstance)
        VulkanHandleGuard::Get().RetireOwner(VulkanHandleGuard::Get().Identity(uint64_t(obj)));
      VulkanHandleGuard::Get().Retire(released);
    }
    delete released;
  }

  // helper for sparse mappings
  void MarkSparseMapReferenced(const ResourceInfo *sparse);

  void SetInternalResource(ResourceId id);

  void MarkMemoryFrameReferenced(ResourceId mem, VkDeviceSize start, VkDeviceSize end,
                                 FrameRefType refType);
  void AddMemoryFrameRefs(ResourceId mem);
  void AddDeviceMemory(ResourceId mem);
  void RemoveDeviceMemory(ResourceId mem);

  void MergeReferencedMemory(std::unordered_map<ResourceId, MemRefs> &memRefs);
  void FixupStorageBufferMemory(const std::unordered_set<VkResourceRecord *> &storageBuffers);
  void ClearReferencedMemory();
  MemRefs *FindMemRefs(ResourceId mem);

  inline InitPolicy GetInitPolicy() { return m_InitPolicy; }
  void SetOptimisationLevel(ReplayOptimisationLevel level)
  {
    switch(level)
    {
      case ReplayOptimisationLevel::Count:
        RDCERR("Invalid optimisation level specified");
        m_InitPolicy = eInitPolicy_NoOpt;
        break;
      case ReplayOptimisationLevel::NoOptimisation: m_InitPolicy = eInitPolicy_NoOpt; break;
      case ReplayOptimisationLevel::Conservative: m_InitPolicy = eInitPolicy_CopyAll; break;
      case ReplayOptimisationLevel::Balanced: m_InitPolicy = eInitPolicy_ClearUnread; break;
      case ReplayOptimisationLevel::Fastest: m_InitPolicy = eInitPolicy_Fastest; break;
    }
  }

  bool IsResourceTrackedForPersistency(WrappedVkRes *const &res);

private:
  bool ResourceTypeRelease(WrappedVkRes *res);

  bool Prepare_InitialState(WrappedVkRes *res);
  void Begin_PrepareInitialBatch();
  void End_PrepareInitialBatch();
  uint64_t GetSize_InitialState(ResourceId id, const VkInitialContents &initial);
  bool Serialise_InitialState(WriteSerialiser &ser, ResourceId id, VkResourceRecord *record,
                              const VkInitialContents *initial);
  void Create_InitialState(ResourceId id, WrappedVkRes *live, bool hasData);
  void Apply_InitialState(WrappedVkRes *live, VkInitialContents &initial);
  rdcarray<ResourceId> InitialContentResources();

  // dummy handle to use - starting from near highest valid pointer to minimise risk of overlap with real handles
  static const uint64_t FirstDummyHandle = UINTPTR_MAX - 1024;
  uint64_t m_DummyHandle = FirstDummyHandle;

  WrappedVulkan *m_Core;
  std::unordered_map<ResourceId, MemRefs> m_MemFrameRefs;
  std::set<ResourceId> m_DeviceMemories;
  rdcarray<ResourceId> m_DeadDeviceMemories;
  InitPolicy m_InitPolicy = eInitPolicy_CopyAll;
};
