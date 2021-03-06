/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <dmlc/memory_io.h>
#include <dmlc/thread_local.h>
#include <tvm/runtime/device_api.h>
#include <tvm/runtime/registry.h>
#include <tvm/target/target.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include "../file_utils.h"
#include "../pack_args.h"
#include "../thread_storage_scope.h"
#include "../workspace_pool.h"
#include "vulkan_common.h"
#include "vulkan_module.h"
#include "vulkan_shader.h"
#include "vulkan_stream.h"

namespace tvm {
namespace runtime {
namespace vulkan {

/*! \brief Maximum number of GPU supported in VulkanModule. */
static constexpr const int kVulkanMaxNumDevice = 8;

/*! \brief TVM Vulkan binary pack magic number */
static constexpr const int kVulkanModuleMagic = 0x02700027;

struct VulkanBuffer {
  VkBuffer buffer{VK_NULL_HANDLE};
  VkDeviceMemory memory{VK_NULL_HANDLE};
};

/*! \brief A struct to represent Vulkan buffers backed by host visible memory */
struct VulkanHostVisibleBuffer {
  // A device where the buffer is allocated
  VkDevice device{nullptr};
  // Vulkan buffer and memory
  VulkanBuffer* vk_buf{nullptr};
  // The corresponding pointer to the host memory
  void* host_addr{nullptr};
  // The size of the buffer in bytes
  size_t size{0};
};

using VulkanStagingBuffer = VulkanHostVisibleBuffer;
using VulkanUniformBuffer = VulkanHostVisibleBuffer;

void DeleteHostVisibleBuffer(VulkanHostVisibleBuffer* buf) {
  if (buf && buf->vk_buf) {
    if (buf->host_addr != nullptr) {
      vkUnmapMemory(buf->device, buf->vk_buf->memory);
    }
    if (buf->vk_buf->memory != VK_NULL_HANDLE) {
      vkFreeMemory(buf->device, buf->vk_buf->memory, nullptr);
    }
    if (buf->vk_buf->buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(buf->device, buf->vk_buf->buffer, nullptr);
    }
    buf->host_addr = nullptr;
    delete buf->vk_buf;
  }
}

class VulkanThreadEntry {
 public:
  VulkanThreadEntry();
  static VulkanThreadEntry* ThreadLocal();

  ~VulkanThreadEntry() {
    // Because the thread entry refers to Device API
    // The command buffer always will be destroyed before
    // the instance and device get destroyed.
    // The destruction need to be manually called
    // to ensure the destruction order.

    pool.reset();
    streams_.clear();
    for (const auto& kv : staging_buffers_) {
      DeleteHostVisibleBuffer(kv.second.get());
    }
  }

  Device device;
  std::unique_ptr<WorkspacePool> pool;
  VulkanStream* Stream(size_t device_id);
  VulkanStagingBuffer* StagingBuffer(int device_id, size_t size);
  void AllocateUniformBuffer(int device_id, size_t size);
  VulkanUniformBuffer* GetUniformBuffer(int device_id, size_t size);

 private:
  std::unordered_map<size_t, std::unique_ptr<VulkanStream>> streams_;
  std::unordered_map<size_t, std::unique_ptr<VulkanStagingBuffer>> staging_buffers_;
  std::unordered_map<size_t, std::unique_ptr<VulkanUniformBuffer>> uniform_buffers_;
};

struct VulkanPipeline {
  VulkanContext* vctx_{nullptr};
  VkShaderModule shader{VK_NULL_HANDLE};
  VkDescriptorSetLayout descriptor_set_layout{VK_NULL_HANDLE};
  VkDescriptorPool descriptor_pool{VK_NULL_HANDLE};
  VkDescriptorSet descriptor_set{VK_NULL_HANDLE};
  VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
  VkPipeline pipeline{VK_NULL_HANDLE};
  VkDescriptorUpdateTemplateKHR descriptor_update_template{VK_NULL_HANDLE};
  bool use_ubo{false};
};

typedef dmlc::ThreadLocalStore<VulkanThreadEntry> VulkanThreadStore;

uint32_t FindMemoryType(const VulkanContext& vctx, VkBufferCreateInfo info,
                        VkMemoryPropertyFlags req_prop) {
  VkBuffer buffer;
  VULKAN_CALL(vkCreateBuffer(vctx.device, &info, nullptr, &buffer));

  VkMemoryRequirements mem_reqs;
  vkGetBufferMemoryRequirements(vctx.device, buffer, &mem_reqs);
  uint32_t type_bits = mem_reqs.memoryTypeBits;
  VkPhysicalDeviceMemoryProperties phy_mem_prop;
  vkGetPhysicalDeviceMemoryProperties(vctx.phy_device, &phy_mem_prop);
  for (uint32_t i = 0; i < phy_mem_prop.memoryTypeCount; i++) {
    if ((type_bits & 1) == 1 &&
        (phy_mem_prop.memoryTypes[i].propertyFlags & req_prop) == req_prop) {
      return i;
    }
    type_bits >>= 1;
  }
  LOG(FATAL) << "Requested memory type not found";
  return 0;
}

VkBufferCreateInfo MakeBufferCreateInfo(const VulkanContext& vctx, size_t nbytes,
                                        VkBufferUsageFlags usage) {
  VkBufferCreateInfo info;
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.pNext = nullptr;
  info.flags = 0;
  info.size = nbytes;
  info.queueFamilyIndexCount = 1;
  info.pQueueFamilyIndices = &(vctx.queue_family_index);
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.usage = usage;
  return info;
}

VulkanBuffer* CreateBuffer(const VulkanContext& vctx, size_t nbytes, VkBufferUsageFlags usage,
                           uint32_t mem_type_index) {
  auto info = MakeBufferCreateInfo(vctx, nbytes, usage);
  // create buffer
  VkBuffer buffer;
  VULKAN_CALL(vkCreateBuffer(vctx.device, &info, nullptr, &buffer));

  // bind to memory
  bool dedicated_allocation = false;
  VkMemoryRequirements2KHR req2;

  if (vctx.get_buffer_memory_requirements_2_functions) {
    VkBufferMemoryRequirementsInfo2KHR req_info2;
    req_info2.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2_KHR;
    req_info2.pNext = 0;
    req_info2.buffer = buffer;

    req2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2_KHR;
    req2.pNext = 0;

    VkMemoryDedicatedRequirementsKHR dedicated_req;
    dedicated_req.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS_KHR;
    dedicated_req.pNext = 0;
    req2.pNext = &dedicated_req;

    vctx.get_buffer_memory_requirements_2_functions->vkGetBufferMemoryRequirements2KHR(
        vctx.device, &req_info2, &req2);
    dedicated_allocation =
        dedicated_req.requiresDedicatedAllocation || dedicated_req.prefersDedicatedAllocation;
  }

  VkDeviceMemory memory;
  if (!dedicated_allocation) {
    VkMemoryAllocateInfo minfo;
    minfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    minfo.pNext = nullptr;
    minfo.allocationSize = info.size;
    minfo.memoryTypeIndex = mem_type_index;
    VULKAN_CALL(vkAllocateMemory(vctx.device, &minfo, nullptr, &memory));
  } else {
    VkMemoryAllocateInfo minfo;
    minfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    minfo.pNext = nullptr;
    minfo.allocationSize = req2.memoryRequirements.size;
    minfo.memoryTypeIndex = mem_type_index;

    VkMemoryDedicatedAllocateInfoKHR mdinfo;
    mdinfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR;
    mdinfo.pNext = 0;
    mdinfo.image = 0;
    mdinfo.buffer = buffer;
    minfo.pNext = &mdinfo;
    VULKAN_CALL(vkAllocateMemory(vctx.device, &minfo, nullptr, &memory));
  }
  VULKAN_CALL(vkBindBufferMemory(vctx.device, buffer, memory, 0));
  VulkanBuffer* pbuf = new VulkanBuffer();
  pbuf->memory = memory;
  pbuf->buffer = buffer;
  return pbuf;
}

class VulkanDeviceAPI final : public DeviceAPI {
 public:
  VulkanDeviceAPI();
  ~VulkanDeviceAPI() {
    for (auto& vctx : context_) {
      vkDestroyDevice(vctx.device, nullptr);
    }
    if (instance_) {
      vkDestroyInstance(instance_, nullptr);
    }
  }
  void SetDevice(Device dev) final { VulkanThreadEntry::ThreadLocal()->device = dev; }
  void GetAttr(Device dev, DeviceAttrKind kind, TVMRetValue* rv) final;
  std::vector<uint32_t> GetComputeQueueFamilies(VkPhysicalDevice phy_dev);
  void* AllocDataSpace(Device dev, size_t nbytes, size_t alignment, DLDataType type_hint) final {
    if (nbytes == 0) {
      // Vulkan seems to have issues if we return nullptr on zero size alloc
      nbytes = 1;
    }
    const auto& vctx = context(dev.device_id);
    auto usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    return CreateBuffer(vctx, nbytes, usage, vctx.compute_mtype_index);
  }

  void FreeDataSpace(Device dev, void* ptr) final {
    // Before releasing the vkBuffer, call sync to
    // finish all the vulkan commands that reference the buffer.
    StreamSync(dev, nullptr);

    const auto& vctx = context(dev.device_id);
    auto* pbuf = static_cast<VulkanBuffer*>(ptr);
    vkDestroyBuffer(vctx.device, pbuf->buffer, nullptr);
    vkFreeMemory(vctx.device, pbuf->memory, nullptr);
    delete pbuf;
  }

  Target GetDeviceDescription(VkInstance instance, VkPhysicalDevice dev,
                              const std::vector<const char*>& instance_extensions,
                              const std::vector<const char*>& device_extensions);

 protected:
  void CopyDataFromTo(const void* from, size_t from_offset, void* to, size_t to_offset, size_t size,
                      Device dev_from, Device dev_to, DLDataType type_hint,
                      TVMStreamHandle stream) final {
    ICHECK(stream == nullptr);
    Device dev = dev_from;
    if (dev_from.device_type == kDLCPU) {
      dev = dev_to;
    }

    int from_dev_type = static_cast<int>(dev_from.device_type);
    int to_dev_type = static_cast<int>(dev_to.device_type);
    if (from_dev_type == kDLVulkan && to_dev_type == kDLVulkan) {
      VulkanThreadEntry::ThreadLocal()
          ->Stream(dev_from.device_id)
          ->Launch([=](VulkanStreamState* state) {
            // 1: copy
            const auto* from_buf = static_cast<const VulkanBuffer*>(from);
            auto* to_buf = static_cast<VulkanBuffer*>(to);
            VkBufferCopy copy_info;
            copy_info.srcOffset = from_offset;
            copy_info.dstOffset = to_offset;
            copy_info.size = size;
            vkCmdCopyBuffer(state->cmd_buffer_, from_buf->buffer, to_buf->buffer, 1, &copy_info);
            // 2: barrier(transfer-> compute|transfer)
            ICHECK_EQ(dev_from.device_id, dev_to.device_id) << "Vulkan disallow cross device copy.";
            VkMemoryBarrier barrier_info;
            barrier_info.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            barrier_info.pNext = nullptr;
            barrier_info.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier_info.dstAccessMask =
                (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                 VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            vkCmdPipelineBarrier(
                state->cmd_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                &barrier_info, 0, nullptr, 0, nullptr);
          });

    } else if (from_dev_type == kDLVulkan && to_dev_type == kDLCPU) {
      const auto* from_buf = static_cast<const VulkanBuffer*>(from);
      const auto& vctx = context(dev_from.device_id);
      auto* temp = VulkanThreadEntry::ThreadLocal()->StagingBuffer(dev_from.device_id, size);
      VulkanThreadEntry::ThreadLocal()
          ->Stream(dev_from.device_id)
          ->Launch([&](VulkanStreamState* state) {
            VkBufferCopy copy_info;
            copy_info.srcOffset = from_offset;
            copy_info.dstOffset = 0;
            copy_info.size = size;
            vkCmdCopyBuffer(state->cmd_buffer_, from_buf->buffer, temp->vk_buf->buffer, 1,
                            &copy_info);
          });
      VulkanThreadEntry::ThreadLocal()->Stream(dev_from.device_id)->Synchronize();
      if (!vctx.coherent_staging) {
        VkMappedMemoryRange mrange;
        mrange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mrange.pNext = nullptr;
        mrange.memory = temp->vk_buf->memory;
        mrange.offset = 0;
        mrange.size = VK_WHOLE_SIZE;  // size;
        VULKAN_CALL(vkInvalidateMappedMemoryRanges(vctx.device, 1, &mrange));
      }
      memcpy(static_cast<char*>(to) + to_offset, static_cast<char*>(temp->host_addr), size);
    } else if (from_dev_type == kDLCPU && to_dev_type == kDLVulkan) {
      const auto& vctx = context(dev_to.device_id);
      const auto* to_buf = static_cast<const VulkanBuffer*>(to);
      VulkanStagingBuffer* temp =
          VulkanThreadEntry::ThreadLocal()->StagingBuffer(dev_to.device_id, size);
      memcpy(temp->host_addr, static_cast<const char*>(from) + from_offset, size);
      // host side flush if access is not coherent.
      // so writes from CPU is visible to GPU
      if (!vctx.coherent_staging) {
        VkMappedMemoryRange mrange;
        mrange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mrange.pNext = nullptr;
        mrange.memory = temp->vk_buf->memory;
        mrange.offset = 0;
        mrange.size = VK_WHOLE_SIZE;  // size;
        VULKAN_CALL(vkFlushMappedMemoryRanges(vctx.device, 1, &mrange));
      }

      VulkanThreadEntry::ThreadLocal()
          ->Stream(dev_to.device_id)
          ->Launch([&](VulkanStreamState* state) {
            // 0: barrier(host->transfer)
            VkMemoryBarrier barrier_info;
            barrier_info.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            barrier_info.pNext = nullptr;
            barrier_info.srcAccessMask = 0;
            barrier_info.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(state->cmd_buffer_, VK_PIPELINE_STAGE_HOST_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier_info, 0, nullptr, 0,
                                 nullptr);
            // 1: copy
            VkBufferCopy copy_info;
            copy_info.srcOffset = 0;
            copy_info.dstOffset = to_offset;
            copy_info.size = size;
            vkCmdCopyBuffer(state->cmd_buffer_, temp->vk_buf->buffer, to_buf->buffer, 1,
                            &copy_info);
          });
      // TODO(tulloch): should we instead make the staging buffer a property of the
      // Stream? This would allow us to elide synchronizations here.
      VulkanThreadEntry::ThreadLocal()->Stream(dev_to.device_id)->Synchronize();
    } else {
      LOG(FATAL) << "Expect copy from/to Vulkan or between Vulkan"
                 << ", from=" << from_dev_type << ", to=" << to_dev_type;
    }
  }

 public:
  // Current vulkan implementation has one "stream" per CPU thread,
  // with all commands writing into a single command buffer that is
  // submitted on a call to StreamSync.  Therefore, for now, these are
  // mostly no-ops.  If needed in the future, could have multiple
  // command buffers to act as multiple streams.
  TVMStreamHandle CreateStream(Device dev) final { return nullptr; }

  void FreeStream(Device dev, TVMStreamHandle stream) final {
    ICHECK_EQ(stream, static_cast<void*>(nullptr));
    return;
  }

  // Syncing two streams is a nop, since there is only one stream.
  void SyncStreamFromTo(Device dev, TVMStreamHandle event_src, TVMStreamHandle event_dst) final {
    ICHECK_EQ(event_src, static_cast<void*>(nullptr));
    ICHECK_EQ(event_dst, static_cast<void*>(nullptr));
    return;
  }

  void StreamSync(Device dev, TVMStreamHandle stream) final {
    ICHECK_EQ(stream, static_cast<void*>(nullptr));
    VulkanThreadEntry::ThreadLocal()->Stream(dev.device_id)->Synchronize();
  }

  void SetStream(Device dev, TVMStreamHandle stream) final {
    ICHECK_EQ(stream, static_cast<void*>(nullptr));
    return;
  }

  void* AllocWorkspace(Device dev, size_t size, DLDataType type_hint) final {
    return VulkanThreadEntry::ThreadLocal()->pool->AllocWorkspace(dev, size);
  }

  void FreeWorkspace(Device dev, void* data) final {
    VulkanThreadEntry::ThreadLocal()->pool->FreeWorkspace(dev, data);
  }

  static VulkanDeviceAPI* Global() {
    // Most of the TVM Global() functions allocate with "new" and do
    // not deallocate, as the OS can clean up any leftover buffers at
    // the end.  In this case, we need the VulkanDeviceAPI destructor
    // to call vkDestroyInstance, to prevent a segfault on exit when
    // using some nvidia drivers.
    static VulkanDeviceAPI inst;
    return &inst;
  }

  const VulkanContext& context(size_t device_id) const {
    ICHECK_LT(device_id, context_.size());
    return context_[device_id];
  }

  Target GenerateTarget(size_t device_id) const { return context(device_id).target; }

 private:
  std::vector<const char*> find_enabled_extensions(
      const std::vector<VkExtensionProperties>& ext_prop,
      const std::vector<const char*>& required_extensions,
      const std::vector<const char*>& optional_extensions) {
    std::set<std::string> available_extensions;
    for (const auto& prop : ext_prop) {
      if (prop.specVersion > 0) {
        available_extensions.insert(prop.extensionName);
      }
    }

    std::vector<const char*> enabled_extensions;
    for (const auto& ext : required_extensions) {
      ICHECK(available_extensions.count(ext))
          << "Required vulkan extension \"" << ext << "\" not supported by driver";
      enabled_extensions.push_back(ext);
    }

    for (const auto& ext : optional_extensions) {
      if (available_extensions.count(ext)) {
        enabled_extensions.push_back(ext);
      }
    }

    return enabled_extensions;
  }

  VkInstance instance_{nullptr};
  // The physical devices, have 1 to 1 mapping to devices
  std::vector<VulkanContext> context_;
};

Target VulkanDeviceAPI::GetDeviceDescription(VkInstance instance, VkPhysicalDevice dev,
                                             const std::vector<const char*>& instance_extensions,
                                             const std::vector<const char*>& device_extensions) {
  auto has_extension = [&](const char* query) {
    return std::any_of(device_extensions.begin(), device_extensions.end(),
                       [&](const char* extension) { return std::strcmp(query, extension) == 0; }) ||
           std::any_of(instance_extensions.begin(), instance_extensions.end(),
                       [&](const char* extension) { return std::strcmp(query, extension) == 0; });
  };

  // Declare output locations for properties
  VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceSubgroupProperties subgroup = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};

  // Need to do initial query in order to check the apiVersion.
  vkGetPhysicalDeviceProperties(dev, &properties.properties);

  // Set up linked list for property query
  {
    void** pp_next = &properties.pNext;
    if (has_extension("VK_KHR_driver_properties")) {
      *pp_next = &driver;
      pp_next = &driver.pNext;
    }
    if (properties.properties.apiVersion >= VK_API_VERSION_1_1) {
      *pp_next = &subgroup;
      pp_next = &subgroup.pNext;
    }
  }

  // Declare output locations for features
  VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDevice8BitStorageFeatures storage_8bit = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES};
  VkPhysicalDevice16BitStorageFeatures storage_16bit = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
  VkPhysicalDeviceShaderFloat16Int8Features float16_int8 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};

  // Set up linked list for feature query
  {
    void** pp_next = &features.pNext;
    if (has_extension("VK_KHR_8bit_storage")) {
      *pp_next = &storage_8bit;
      pp_next = &storage_8bit.pNext;
    }
    if (has_extension("VK_KHR_16bit_storage")) {
      *pp_next = &storage_16bit;
      pp_next = &storage_16bit.pNext;
    }
    if (has_extension("VK_KHR_shader_float16_int8")) {
      *pp_next = &float16_int8;
      pp_next = &float16_int8.pNext;
    }
  }

  if (has_extension("VK_KHR_get_physical_device_properties2")) {
    // Preferred method, call to get all properties that can be queried.
    auto vkGetPhysicalDeviceProperties2KHR = (PFN_vkGetPhysicalDeviceProperties2KHR)ICHECK_NOTNULL(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2KHR"));
    vkGetPhysicalDeviceProperties2KHR(dev, &properties);

    auto vkGetPhysicalDeviceFeatures2KHR = (PFN_vkGetPhysicalDeviceFeatures2KHR)ICHECK_NOTNULL(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR"));
    vkGetPhysicalDeviceFeatures2KHR(dev, &features);
  } else {
    // Fallback, get as many features as we can from the Vulkan1.0
    // API.  Corresponding vkGetPhysicalDeviceProperties was already done earlier.
    vkGetPhysicalDeviceFeatures(dev, &features.features);
  }

  //// Now, extracting all the information from the vulkan query.

  // Not technically needed, because VK_SHADER_STAGE_COMPUTE_BIT will
  // be set so long at least one queue has VK_QUEUE_COMPUTE_BIT, but
  // preferring the explicit check.
  uint32_t supported_subgroup_operations =
      (subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ? subgroup.supportedOperations : 0;

  // Even if we can't query it, warp size must be at least 1.  Must
  // also be defined, as `transpose` operation requires it.
  uint32_t thread_warp_size = std::max(subgroup.subgroupSize, 1U);

  // By default, use the maximum API version that the driver allows,
  // so that any supported features can be used by TVM shaders.
  // However, if we can query the conformance version, then limit to
  // only using the api version that passes the vulkan conformance
  // tests.
  uint32_t vulkan_api_version = properties.properties.apiVersion;
  if (has_extension("VK_KHR_driver_properties")) {
    auto api_major = VK_VERSION_MAJOR(vulkan_api_version);
    auto api_minor = VK_VERSION_MINOR(vulkan_api_version);
    if ((api_major > driver.conformanceVersion.major) ||
        ((api_major == driver.conformanceVersion.major) &&
         (api_minor > driver.conformanceVersion.minor))) {
      vulkan_api_version =
          VK_MAKE_VERSION(driver.conformanceVersion.major, driver.conformanceVersion.minor, 0);
    }
  }

  // From "Versions and Formats" section of Vulkan spec.
  uint32_t max_spirv_version = 0x10000;
  if (vulkan_api_version >= VK_API_VERSION_1_2) {
    max_spirv_version = 0x10500;
  } else if (has_extension("VK_KHR_spirv_1_4")) {
    max_spirv_version = 0x10400;
  } else if (vulkan_api_version >= VK_API_VERSION_1_1) {
    max_spirv_version = 0x10300;
  }

  // Support is available based on these extensions, but allow it to
  // be disabled based on an environment variable.
  bool supports_push_descriptor =
      has_extension("VK_KHR_push_descriptor") && has_extension("VK_KHR_descriptor_update_template");
  {
    const char* disable = std::getenv("TVM_VULKAN_DISABLE_PUSH_DESCRIPTOR");
    if (disable && *disable) {
      supports_push_descriptor = false;
    }
  }

  // Support is available based on these extensions, but allow it to
  // be disabled based on an environment variable.
  bool supports_dedicated_allocation = has_extension("VK_KHR_get_memory_requirements2") &&
                                       has_extension("VK_KHR_dedicated_allocation");
  {
    const char* disable = std::getenv("TVM_VULKAN_DISABLE_DEDICATED_ALLOCATION");
    if (disable && *disable) {
      supports_dedicated_allocation = false;
    }
  }

  Map<String, ObjectRef> config = {
      {"kind", String("vulkan")},
      // Feature support
      {"supports_float16", Bool(float16_int8.shaderFloat16)},
      {"supports_float32", Bool(true)},
      {"supports_float64", Bool(features.features.shaderFloat64)},
      {"supports_int8", Bool(float16_int8.shaderInt8)},
      {"supports_int16", Bool(features.features.shaderInt16)},
      {"supports_int32", Bool(true)},
      {"supports_int64", Bool(features.features.shaderInt64)},
      {"supports_8bit_buffer", Bool(storage_8bit.storageBuffer8BitAccess)},
      {"supports_16bit_buffer", Bool(storage_16bit.storageBuffer16BitAccess)},
      {"supports_storage_buffer_storage_class",
       Bool(has_extension("VK_KHR_storage_buffer_storage_class"))},
      {"supports_push_descriptor", Bool(supports_push_descriptor)},
      {"supports_dedicated_allocation", Bool(supports_dedicated_allocation)},
      {"supported_subgroup_operations", Integer(supported_subgroup_operations)},
      // Physical device limits
      {"max_num_threads", Integer(properties.properties.limits.maxComputeWorkGroupInvocations)},
      {"thread_warp_size", Integer(thread_warp_size)},
      {"max_block_size_x", Integer(properties.properties.limits.maxComputeWorkGroupSize[0])},
      {"max_block_size_y", Integer(properties.properties.limits.maxComputeWorkGroupSize[1])},
      {"max_block_size_z", Integer(properties.properties.limits.maxComputeWorkGroupSize[2])},
      {"max_push_constants_size", Integer(properties.properties.limits.maxPushConstantsSize)},
      {"max_uniform_buffer_range", Integer(properties.properties.limits.maxUniformBufferRange)},
      {"max_storage_buffer_range",
       Integer(IntImm(DataType::UInt(32), properties.properties.limits.maxStorageBufferRange))},
      {"max_per_stage_descriptor_storage_buffer",
       Integer(properties.properties.limits.maxPerStageDescriptorStorageBuffers)},
      {"max_shared_memory_per_block",
       Integer(properties.properties.limits.maxComputeSharedMemorySize)},
      // Other device properties
      {"device_name", String(properties.properties.deviceName)},
      {"driver_version", Integer(properties.properties.driverVersion)},
      {"vulkan_api_version", Integer(vulkan_api_version)},
      {"max_spirv_version", Integer(max_spirv_version)},
  };

  return Target(config);
}

void VulkanDeviceAPI::GetAttr(Device dev, DeviceAttrKind kind, TVMRetValue* rv) {
  size_t index = static_cast<size_t>(dev.device_id);
  if (kind == kExist) {
    *rv = static_cast<int>(index < context_.size());
    return;
  }
  ICHECK_LT(index, context_.size()) << "Invalid device id " << index;

  const auto& target = context(index).target;

  switch (kind) {
    case kMaxThreadsPerBlock: {
      *rv = target->GetAttr<Integer>("max_num_threads").value();
      break;
    }
    case kMaxSharedMemoryPerBlock: {
      *rv = target->GetAttr<Integer>("max_shared_memory_per_block");
      break;
    }
    case kWarpSize: {
      *rv = target->GetAttr<Integer>("thread_warp_size").value();
      break;
    }
    case kComputeVersion: {
      int64_t value = target->GetAttr<Integer>("vulkan_api_version").value();
      std::ostringstream os;
      os << VK_VERSION_MAJOR(value) << "." << VK_VERSION_MINOR(value) << "."
         << VK_VERSION_PATCH(value);
      *rv = os.str();
      break;
    }
    case kDeviceName:
      *rv = target->GetAttr<String>("device_name").value();
      break;

    case kMaxClockRate:
      break;

    case kMultiProcessorCount:
      break;

    case kExist:
      break;

    case kMaxThreadDimensions: {
      std::stringstream ss;  // use json string to return multiple int values;
      ss << "[" << target->GetAttr<Integer>("max_block_size_x").value() << ", "
         << target->GetAttr<Integer>("max_block_size_y").value() << ", "
         << target->GetAttr<Integer>("max_block_size_z").value() << "]";
      *rv = ss.str();
      break;
    }

    case kMaxRegistersPerBlock:
      break;

    case kGcnArch:
      break;

    case kApiVersion:
      *rv = VK_HEADER_VERSION;
      break;

    case kDriverVersion: {
      int64_t value = target->GetAttr<Integer>("driver_version").value();
      std::ostringstream os;
      os << VK_VERSION_MAJOR(value) << "." << VK_VERSION_MINOR(value) << "."
         << VK_VERSION_PATCH(value);
      *rv = os.str();
      break;
    }
  }
}

VulkanDeviceAPI::VulkanDeviceAPI() {
  const auto layers = []() -> std::vector<const char*> {
    uint32_t inst_layer_prop_count;
    VULKAN_CALL(vkEnumerateInstanceLayerProperties(&inst_layer_prop_count, nullptr));
    std::vector<VkLayerProperties> inst_layer_prop(inst_layer_prop_count);
    VULKAN_CALL(vkEnumerateInstanceLayerProperties(&inst_layer_prop_count, inst_layer_prop.data()));
    std::vector<const char*> l;

    const char* enable = std::getenv("TVM_VULKAN_ENABLE_VALIDATION_LAYERS");
    bool validation_enabled = enable && *enable;
    if (validation_enabled) {
      for (const auto& lp : inst_layer_prop) {
        if (std::strcmp(lp.layerName, "VK_LAYER_LUNARG_standard_validation") == 0) {
          l.push_back("VK_LAYER_LUNARG_standard_validation");
        }
        if (std::strcmp(lp.layerName, "VK_LAYER_LUNARG_parameter_validation") == 0) {
          l.push_back("VK_LAYER_LUNARG_parameter_validation");
        }
        if (std::strcmp(lp.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
          l.push_back("VK_LAYER_KHRONOS_validation");
        }
      }
    }
    return l;
  }();

  const auto instance_extensions = [this]() {
    std::vector<const char*> required_extensions{};
    std::vector<const char*> optional_extensions{"VK_KHR_get_physical_device_properties2"};

    uint32_t inst_extension_prop_count;
    VULKAN_CALL(
        vkEnumerateInstanceExtensionProperties(nullptr, &inst_extension_prop_count, nullptr));
    std::vector<VkExtensionProperties> inst_extension_prop(inst_extension_prop_count);
    VULKAN_CALL(vkEnumerateInstanceExtensionProperties(nullptr, &inst_extension_prop_count,
                                                       inst_extension_prop.data()));

    return find_enabled_extensions(inst_extension_prop, required_extensions, optional_extensions);
  }();

  auto has_instance_extension = [&instance_extensions](const char* query) {
    return std::any_of(instance_extensions.begin(), instance_extensions.end(),
                       [&](const char* extension) { return std::strcmp(query, extension) == 0; });
  };

  const auto instance_api_version = []() {
    uint32_t api_version = VK_MAKE_VERSION(1, 0, 0);

    // Result from vkGetInstanceProcAddr is NULL if driver only
    // supports vulkan 1.0.
    auto vkEnumerateInstanceVersion =
        (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    if (vkEnumerateInstanceVersion) {
      vkEnumerateInstanceVersion(&api_version);
    }

    return api_version;
  }();

  {
    VkApplicationInfo app_info;
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pNext = nullptr;
    app_info.pApplicationName = "TVM";
    app_info.applicationVersion = 0;
    app_info.pEngineName = "";
    app_info.engineVersion = 0;
    app_info.apiVersion = instance_api_version;

    VkInstanceCreateInfo inst_info;
    inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_info.pNext = nullptr;
    inst_info.flags = 0;
    inst_info.pApplicationInfo = &app_info;
    inst_info.enabledLayerCount = layers.size();
    inst_info.ppEnabledLayerNames = layers.data();
    inst_info.enabledExtensionCount = instance_extensions.size();
    inst_info.ppEnabledExtensionNames = instance_extensions.data();

    VULKAN_CALL(vkCreateInstance(&inst_info, nullptr, &instance_));
  }

  uint32_t phy_dev_count = 0;
  VULKAN_CALL(vkEnumeratePhysicalDevices(instance_, &phy_dev_count, nullptr));
  std::vector<VkPhysicalDevice> all_phy_devs(phy_dev_count);
  VULKAN_CALL(vkEnumeratePhysicalDevices(instance_, &phy_dev_count, dmlc::BeginPtr(all_phy_devs)));
  for (VkPhysicalDevice phy_dev : all_phy_devs) {
    // Get a list of queue families supporting compute, in order of preference. We currently only
    // make use of the most preferred one family.
    std::vector<uint32_t> queue_family_indexes = GetComputeQueueFamilies(phy_dev);
    if (queue_family_indexes.empty()) continue;
    uint32_t queue_family_index = queue_family_indexes[0];
    float priority = 1.0f;

    struct VkDeviceQueueCreateInfo queue_create_info;
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.pNext = nullptr;
    queue_create_info.flags = 0;
    queue_create_info.queueFamilyIndex = queue_family_index;
    queue_create_info.queueCount = 1;
    queue_create_info.pQueuePriorities = &priority;

    VulkanContext ctx;
    // setup context
    ctx.phy_device = phy_dev;
    vkGetPhysicalDeviceProperties(ctx.phy_device, &(ctx.phy_device_prop));

    const auto device_extensions = [&]() {
      std::vector<const char*> required_extensions{};
      std::vector<const char*> optional_extensions{
          "VK_KHR_driver_properties",
          "VK_KHR_storage_buffer_storage_class",
          "VK_KHR_8bit_storage",
          "VK_KHR_16bit_storage",
          "VK_KHR_shader_float16_int8",
          "VK_KHR_push_descriptor",
          "VK_KHR_descriptor_update_template",
          "VK_KHR_get_memory_requirements2",
          "VK_KHR_dedicated_allocation",
          "VK_KHR_spirv_1_4",
      };

      uint32_t device_extension_prop_count;
      VULKAN_CALL(vkEnumerateDeviceExtensionProperties(ctx.phy_device, nullptr,
                                                       &device_extension_prop_count, nullptr));
      std::vector<VkExtensionProperties> device_extension_prop(device_extension_prop_count);
      VULKAN_CALL(vkEnumerateDeviceExtensionProperties(
          ctx.phy_device, nullptr, &device_extension_prop_count, device_extension_prop.data()));

      return find_enabled_extensions(device_extension_prop, required_extensions,
                                     optional_extensions);
    }();

    ctx.target = GetDeviceDescription(instance_, phy_dev, instance_extensions, device_extensions);

    {
      // Enable all features we may use that a device supports.
      VkPhysicalDeviceFeatures2 enabled_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      VkPhysicalDevice8BitStorageFeatures storage_8bit = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES};
      VkPhysicalDevice16BitStorageFeatures storage_16bit = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
      VkPhysicalDeviceShaderFloat16Int8Features float16_int8 = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};

      void** pp_next = &enabled_features.pNext;
      bool needs_float16_int8 = false;

      auto has_support = [&](const char* name) { return ctx.target->GetAttr<Bool>(name).value(); };
      if (has_support("supports_float16")) {
        float16_int8.shaderFloat16 = true;
        needs_float16_int8 = true;
      }
      if (has_support("supports_float64")) {
        enabled_features.features.shaderFloat64 = true;
      }
      if (has_support("supports_int8")) {
        float16_int8.shaderInt8 = true;
        needs_float16_int8 = true;
      }
      if (has_support("supports_int16")) {
        enabled_features.features.shaderInt16 = true;
      }
      if (has_support("supports_int64")) {
        enabled_features.features.shaderInt64 = true;
      }
      if (has_support("supports_8bit_buffer")) {
        storage_8bit.storageBuffer8BitAccess = true;
        *pp_next = &storage_8bit;
        pp_next = &storage_8bit.pNext;
      }
      if (has_support("supports_16bit_buffer")) {
        storage_16bit.storageBuffer16BitAccess = true;
        *pp_next = &storage_16bit;
        pp_next = &storage_16bit.pNext;
      }

      if (needs_float16_int8) {
        *pp_next = &float16_int8;
        pp_next = &float16_int8.pNext;
      }

      VkDeviceCreateInfo device_create_info;
      device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
      device_create_info.pNext = nullptr;
      device_create_info.flags = 0;
      device_create_info.queueCreateInfoCount = 1;
      device_create_info.pQueueCreateInfos = &queue_create_info;
      device_create_info.enabledLayerCount = 0;
      device_create_info.ppEnabledLayerNames = nullptr;
      device_create_info.enabledExtensionCount = device_extensions.size();
      device_create_info.ppEnabledExtensionNames = device_extensions.data();

      if (has_instance_extension("VK_KHR_get_physical_device_properties2")) {
        device_create_info.pEnabledFeatures = nullptr;
        device_create_info.pNext = &enabled_features;
      } else {
        device_create_info.pNext = nullptr;
        device_create_info.pEnabledFeatures = &enabled_features.features;
      }
      VULKAN_CALL(vkCreateDevice(phy_dev, &device_create_info, nullptr, &(ctx.device)));
    }

    ctx.queue_mutex.reset(new std::mutex());
    vkGetDeviceQueue(ctx.device, queue_family_index, 0, &(ctx.queue));
    ctx.queue_family_index = queue_family_index;
    // Find suitable memory type for staging and compute
    // Find suitable compute index.
    VkBuffer buffer;
    VkMemoryRequirements req_staging, req_compute;
    VkBufferCreateInfo info;
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.pNext = nullptr;
    info.flags = 0;
    info.size = 1024;
    info.queueFamilyIndexCount = 1;
    info.pQueueFamilyIndices = &(ctx.queue_family_index);
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // get staging requirement
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VULKAN_CALL(vkCreateBuffer(ctx.device, &info, nullptr, &buffer));
    vkGetBufferMemoryRequirements(ctx.device, buffer, &req_staging);
    vkDestroyBuffer(ctx.device, buffer, nullptr);
    // get compute requirement
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VULKAN_CALL(vkCreateBuffer(ctx.device, &info, nullptr, &buffer));
    vkGetBufferMemoryRequirements(ctx.device, buffer, &req_compute);
    vkDestroyBuffer(ctx.device, buffer, nullptr);

    // Query phyiscal device property
    // find a memory that is host visible, no need to be consistent
    int win_rank = -1;
    VkPhysicalDeviceMemoryProperties prop;
    vkGetPhysicalDeviceMemoryProperties(ctx.phy_device, &prop);

    for (uint32_t k = 0; k < prop.memoryTypeCount; ++k) {
      VkMemoryType ty = prop.memoryTypes[k];
      size_t heap_size = prop.memoryHeaps[ty.heapIndex].size;
      // host visible
      if (!(ty.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
      // match copy requirment
      if (!(req_staging.memoryTypeBits & (1 << k))) continue;
      if (heap_size < 1024) continue;
      int rank = 0;
      rank += ty.propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      if (rank > win_rank) {
        win_rank = rank;
        ctx.staging_mtype_index = k;
        ctx.coherent_staging = ty.propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      }
    }
    ICHECK_GE(win_rank, 0) << "Cannot find suitable staging memory on device.";

    win_rank = -1;
    for (uint32_t k = 0; k < prop.memoryTypeCount; ++k) {
      VkMemoryType ty = prop.memoryTypes[k];
      size_t heap_size = prop.memoryHeaps[ty.heapIndex].size;
      // host visible
      if (!(ty.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
      // match copy requirment
      if (!(req_staging.memoryTypeBits & (1 << k))) continue;
      if (heap_size < 1024) continue;
      int rank = 0;
      // prefer not host visible
      rank += !(ty.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
      if (rank > win_rank) {
        win_rank = rank;
        ctx.compute_mtype_index = k;
      }
    }
    ICHECK_GE(win_rank, 0) << "Cannot find suitable local memory on device.";

    if (ctx.target->GetAttr<Bool>("supports_push_descriptor").value()) {
      ctx.descriptor_template_khr_functions =
          std::make_unique<VulkanDescriptorTemplateKHRFunctions>(ctx.device);
    }

    if (ctx.target->GetAttr<Bool>("supports_dedicated_allocation").value()) {
      ctx.get_buffer_memory_requirements_2_functions =
          std::make_unique<VulkanGetBufferMemoryRequirements2Functions>(ctx.device);
    }

    context_.push_back(std::move(ctx));
  }

  LOG(INFO) << "Initialize Vulkan with " << context_.size() << " devices..";
  for (size_t i = 0; i < context_.size(); ++i) {
    LOG(INFO) << "vulkan(" << i << ")=\'" << context_[i].phy_device_prop.deviceName
              << "\' phy_dev_id=" << context_[i].phy_device
              << " use_immediate=" << context_[i].UseImmediate();
  }
}

std::vector<uint32_t> VulkanDeviceAPI::GetComputeQueueFamilies(VkPhysicalDevice phy_dev) {
  uint32_t queue_prop_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phy_dev, &queue_prop_count, nullptr);
  std::vector<VkQueueFamilyProperties> queue_props(queue_prop_count);
  vkGetPhysicalDeviceQueueFamilyProperties(phy_dev, &queue_prop_count, dmlc::BeginPtr(queue_props));

  std::vector<uint32_t> result;
  // Prefer compute-only queues. On cerain devices supporting this (e.g. Mesa RADV), using
  // compute-only queues gives better responsiveness for other graphics workload (e.g. desktop).
  for (uint32_t i = 0; i != queue_prop_count; ++i) {
    if ((VK_QUEUE_COMPUTE_BIT & queue_props[i].queueFlags) != 0 &&
        (VK_QUEUE_GRAPHICS_BIT & queue_props[i].queueFlags) == 0) {
      result.push_back(i);
    }
  }
  // Now, push the compute queues that we skipped above into the list.
  for (uint32_t i = 0; i != queue_prop_count; ++i) {
    if ((VK_QUEUE_COMPUTE_BIT & queue_props[i].queueFlags) != 0 &&
        (VK_QUEUE_GRAPHICS_BIT & queue_props[i].queueFlags) != 0) {
      result.push_back(i);
    }
  }
  return result;
}

// namespace vulkan
class VulkanModuleNode;

// a wrapped function class to get packed func.
class VulkanWrappedFunc {
 public:
  void Init(VulkanModuleNode* m, ObjectPtr<Object> sptr, const std::string& func_name,
            size_t num_buffer_args, size_t num_pack_args,
            const std::vector<std::string>& thread_axis_tags) {
    m_ = m;
    sptr_ = sptr;
    func_name_ = func_name;
    num_buffer_args_ = num_buffer_args;
    num_pack_args_ = num_pack_args;
    thread_axis_cfg_.Init(num_buffer_args + num_pack_args, thread_axis_tags);
  }

  void operator()(TVMArgs args, TVMRetValue* rv, const ArgUnion64* pack_args) const;

 private:
  // internal module
  VulkanModuleNode* m_;
  // the resource holder
  ObjectPtr<Object> sptr_;
  // v The name of the function.
  std::string func_name_;
  // Number of buffer arguments
  size_t num_buffer_args_;
  // number of packed arguments.
  size_t num_pack_args_;
  // Device state cache per device.
  // mark as mutable, to enable lazy initialization
  // thread axis configuration
  ThreadAxisConfig thread_axis_cfg_;

  mutable std::array<std::shared_ptr<VulkanPipeline>, kVulkanMaxNumDevice> scache_;
};

// Multi-device enabled module.
class VulkanModuleNode final : public runtime::ModuleNode {
 public:
  explicit VulkanModuleNode(std::unordered_map<std::string, VulkanShader> smap,
                            std::unordered_map<std::string, FunctionInfo> fmap, std::string source)
      : smap_(smap), fmap_(fmap), source_(source) {}

  const char* type_key() const final { return "vulkan"; }

  PackedFunc GetFunction(const std::string& name, const ObjectPtr<Object>& sptr_to_self) final {
    ICHECK_EQ(sptr_to_self.get(), this);
    ICHECK_NE(name, symbol::tvm_module_main) << "Device function do not have main";
    auto it = fmap_.find(name);
    if (it == fmap_.end()) return PackedFunc();
    const FunctionInfo& info = it->second;
    VulkanWrappedFunc f;
    size_t num_buffer_args = NumBufferArgs(info.arg_types);
    f.Init(this, sptr_to_self, name, num_buffer_args, info.arg_types.size() - num_buffer_args,
           info.thread_axis_tags);
    return PackFuncNonBufferArg(std::move(f), info.arg_types);
  }

  ~VulkanModuleNode() {
    // cleanup vulkan related caches.
    for (size_t device_id = 0; device_id < ecache_.size(); ++device_id) {
      for (auto& kv : ecache_[device_id]) {
        auto& pe = kv.second;
        ICHECK(pe);
        const auto& vctx = VulkanDeviceAPI::Global()->context(device_id);

        if (pe->descriptor_update_template != VK_NULL_HANDLE) {
          vctx.descriptor_template_khr_functions->vkDestroyDescriptorUpdateTemplateKHR(
              vctx.device, pe->descriptor_update_template, nullptr);
        }
        vkDestroyPipeline(vctx.device, pe->pipeline, nullptr);
        vkDestroyPipelineLayout(vctx.device, pe->pipeline_layout, nullptr);
        vkDestroyDescriptorPool(vctx.device, pe->descriptor_pool, nullptr);
        vkDestroyDescriptorSetLayout(vctx.device, pe->descriptor_set_layout, nullptr);
        vkDestroyShaderModule(vctx.device, pe->shader, nullptr);
      }
    }
  }

  std::shared_ptr<VulkanPipeline> GetPipeline(size_t device_id, const std::string& func_name,
                                              size_t num_pack_args) {
    const auto& vctx = VulkanDeviceAPI::Global()->context(device_id);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& cp = ecache_[device_id][func_name];
    if (cp) {
      return cp;
    }
    // Create new pipeline
    auto pe = std::make_shared<VulkanPipeline>();
    {
      // create shader
      auto sit = smap_.find(func_name);
      ICHECK(sit != smap_.end());
      pe->use_ubo = sit->second.flag & (1 << ShaderMetaDataFlagMask::kUseUBO);
      const std::vector<uint32_t>& data = sit->second.data;
      VkShaderModuleCreateInfo shader_cinfo;
      shader_cinfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      shader_cinfo.pNext = nullptr;
      shader_cinfo.flags = 0;
      shader_cinfo.codeSize = data.size() * sizeof(uint32_t);
      shader_cinfo.pCode = data.data();
      VULKAN_CALL(vkCreateShaderModule(vctx.device, &shader_cinfo, nullptr, &(pe->shader)));
    }
    std::vector<VkDescriptorSetLayoutBinding> arg_binding;
    std::vector<VkDescriptorUpdateTemplateEntryKHR> arg_template;
    std::vector<VkDescriptorPoolSize> descriptor_set_pool_sizes;
    uint32_t num_pod = 0, num_buffer = 0;

    auto push_arg_info = [&arg_binding, &arg_template, &descriptor_set_pool_sizes](
                             uint32_t binding, VkDescriptorType desc_type) {
      {
        auto result =
            std::find_if(descriptor_set_pool_sizes.begin(), descriptor_set_pool_sizes.end(),
                         [&](const auto& psize) { return psize.type == desc_type; });
        if (result == descriptor_set_pool_sizes.end()) {
          VkDescriptorPoolSize new_size;
          new_size.type = desc_type;
          new_size.descriptorCount = 1;
          descriptor_set_pool_sizes.push_back(new_size);
        } else {
          result->descriptorCount++;
        }
      }

      {
        VkDescriptorSetLayoutBinding bd;
        bd.binding = binding;
        bd.descriptorType = desc_type;
        bd.descriptorCount = 1;
        bd.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bd.pImmutableSamplers = nullptr;
        arg_binding.push_back(bd);
      }
      {
        VkDescriptorUpdateTemplateEntryKHR tpl;
        tpl.dstBinding = binding;
        tpl.dstArrayElement = 0;
        tpl.descriptorCount = 1;
        tpl.descriptorType = desc_type;
        tpl.offset = binding * sizeof(VkDescriptorBufferInfo);
        tpl.stride = sizeof(VkDescriptorBufferInfo);
        arg_template.push_back(tpl);
      }
    };

    {
      auto fit = fmap_.find(func_name);
      ICHECK(fit != fmap_.end());
      for (DLDataType arg_type : fit->second.arg_types) {
        if (arg_type.code == kTVMOpaqueHandle) {
          push_arg_info(num_buffer, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
          ++num_buffer;
        } else {
          ++num_pod;
        }
      }
    }

    size_t nbytes_scalars = num_pod * sizeof(ArgUnion64);
    if (pe->use_ubo) {
      // Use UBO instead of push constants
      push_arg_info(num_buffer, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
      VulkanThreadEntry::ThreadLocal()->AllocateUniformBuffer(device_id, nbytes_scalars);
    }

    {
      VkDescriptorSetLayoutCreateInfo descrip_cinfo;
      descrip_cinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      descrip_cinfo.pNext = nullptr;
      descrip_cinfo.flags = 0;
      if (vctx.UseImmediate()) {
        descrip_cinfo.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
      }
      descrip_cinfo.bindingCount = arg_binding.size();
      descrip_cinfo.pBindings = arg_binding.data();
      VULKAN_CALL(vkCreateDescriptorSetLayout(vctx.device, &descrip_cinfo, nullptr,
                                              &(pe->descriptor_set_layout)));
    }

    if (!vctx.UseImmediate()) {
      VkDescriptorPoolCreateInfo descrip_pool_cinfo;
      descrip_pool_cinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      descrip_pool_cinfo.pNext = nullptr;
      descrip_pool_cinfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
      descrip_pool_cinfo.maxSets = 1;
      descrip_pool_cinfo.poolSizeCount = descriptor_set_pool_sizes.size();
      descrip_pool_cinfo.pPoolSizes = descriptor_set_pool_sizes.data();
      VULKAN_CALL(vkCreateDescriptorPool(vctx.device, &descrip_pool_cinfo, nullptr,
                                         &(pe->descriptor_pool)));

      VkDescriptorSetAllocateInfo alloc_info;
      alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      alloc_info.pNext = nullptr;
      alloc_info.descriptorPool = pe->descriptor_pool;
      alloc_info.descriptorSetCount = 1;
      alloc_info.pSetLayouts = &(pe->descriptor_set_layout);
      VULKAN_CALL(vkAllocateDescriptorSets(vctx.device, &alloc_info, &(pe->descriptor_set)));
    }

    VkPushConstantRange crange;
    crange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    crange.offset = 0;
    crange.size = sizeof(ArgUnion64) * num_pack_args;

    VkPipelineLayoutCreateInfo playout_cinfo;
    playout_cinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    playout_cinfo.pNext = nullptr;
    playout_cinfo.flags = 0;
    playout_cinfo.setLayoutCount = 1;
    playout_cinfo.pSetLayouts = &(pe->descriptor_set_layout);

    if (0 < nbytes_scalars && !pe->use_ubo) {
      playout_cinfo.pushConstantRangeCount = 1;
      playout_cinfo.pPushConstantRanges = &crange;
      ICHECK_LE(crange.size, vctx.phy_device_prop.limits.maxPushConstantsSize);
    } else {
      playout_cinfo.pushConstantRangeCount = 0;
      playout_cinfo.pPushConstantRanges = nullptr;
    }

    VULKAN_CALL(
        vkCreatePipelineLayout(vctx.device, &playout_cinfo, nullptr, &(pe->pipeline_layout)));

    VkComputePipelineCreateInfo pipeline_cinfo;
    pipeline_cinfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_cinfo.pNext = nullptr;
    pipeline_cinfo.flags = 0;
    pipeline_cinfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_cinfo.stage.pNext = nullptr;
    pipeline_cinfo.stage.flags = 0;
    pipeline_cinfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_cinfo.stage.module = pe->shader;
    pipeline_cinfo.stage.pName = func_name.c_str();
    pipeline_cinfo.stage.pSpecializationInfo = nullptr;
    pipeline_cinfo.layout = pe->pipeline_layout;
    pipeline_cinfo.basePipelineHandle = VK_NULL_HANDLE;
    pipeline_cinfo.basePipelineIndex = 0;
    VULKAN_CALL(vkCreateComputePipelines(vctx.device, VK_NULL_HANDLE, 1, &pipeline_cinfo, nullptr,
                                         &(pe->pipeline)));

    if (vctx.UseImmediate()) {
      VkDescriptorUpdateTemplateCreateInfoKHR descrip_template_cinfo;
      descrip_template_cinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO_KHR;
      descrip_template_cinfo.pNext = 0;
      descrip_template_cinfo.flags = 0;
      descrip_template_cinfo.descriptorUpdateEntryCount = arg_template.size();
      descrip_template_cinfo.pDescriptorUpdateEntries = arg_template.data();
      descrip_template_cinfo.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR;
      descrip_template_cinfo.descriptorSetLayout = pe->descriptor_set_layout;
      descrip_template_cinfo.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
      descrip_template_cinfo.pipelineLayout = pe->pipeline_layout;
      descrip_template_cinfo.set = 0;
      VULKAN_CALL(vctx.descriptor_template_khr_functions->vkCreateDescriptorUpdateTemplateKHR(
          vctx.device, &descrip_template_cinfo, 0, &(pe->descriptor_update_template)));
    }
    ecache_[device_id][func_name] = pe;
    return pe;
  }

  void SaveToFile(const std::string& file_name, const std::string& format) final {
    std::string fmt = GetFileFormat(file_name, format);
    ICHECK_EQ(fmt, fmt_) << "Can only save to customized format vulkan";
    std::string meta_file = GetMetaFilePath(file_name);
    SaveMetaDataToFile(meta_file, fmap_);
    std::string data_bin;
    dmlc::MemoryStringStream fs(&data_bin);
    dmlc::Stream* stream = &fs;
    uint32_t magic = kVulkanModuleMagic;
    stream->Write(magic);
    stream->Write(smap_);
    SaveBinaryToFile(file_name, data_bin);
  }

  void SaveToBinary(dmlc::Stream* stream) final {
    stream->Write(fmt_);
    stream->Write(fmap_);
    stream->Write(smap_);
  }
  std::string GetSource(const std::string& format) final {
    // can only return source code.
    return source_;
  }

 private:
  // function information table.
  std::unordered_map<std::string, VulkanShader> smap_;
  // function information table.
  std::unordered_map<std::string, FunctionInfo> fmap_;
  // The format
  std::string fmt_{"vulkan"};
  // The source
  std::string source_;

  // Guards accesses to `ecache_`
  std::mutex mutex_;
  std::array<std::unordered_map<std::string, std::shared_ptr<VulkanPipeline>>, kVulkanMaxNumDevice>
      ecache_;
};

Module VulkanModuleCreate(std::unordered_map<std::string, VulkanShader> smap,
                          std::unordered_map<std::string, FunctionInfo> fmap, std::string source) {
  auto n = make_object<VulkanModuleNode>(smap, fmap, source);
  return Module(n);
}

VulkanThreadEntry* VulkanThreadEntry::ThreadLocal() { return VulkanThreadStore::Get(); }

VulkanHostVisibleBuffer* GetOrAllocate(
    int device_id, size_t size, VkBufferUsageFlags usage, uint32_t mem_type_index,
    std::unordered_map<size_t, std::unique_ptr<VulkanHostVisibleBuffer>>* buffers_ptr,
    bool sync_before_realloc = false) {
  auto& buffers = *buffers_ptr;
  if (!buffers[device_id]) {
    buffers[device_id] = std::make_unique<VulkanHostVisibleBuffer>();
  }

  auto& buf = *(buffers[device_id]);
  if (buf.device != nullptr && buf.size < size) {
    // free previous buffer
    if (sync_before_realloc) {
      // For the deferred execution mode, we need to make sure that old tasks that use
      // the older, smaller buffer get finished
      // Synchronization on staging buffers is done after host to device memory copy
      // For UBO, we sync here before we reallocate a larger buffer, to minimize synchronization
      // points
      VulkanThreadEntry::ThreadLocal()->Stream(device_id)->Synchronize();
    }
    DeleteHostVisibleBuffer(&buf);
  }

  const auto& vctx = VulkanDeviceAPI::Global()->context(device_id);

  if (buf.device == nullptr) {
    buf.device = vctx.device;
  }
  if (buf.host_addr == nullptr) {
    buf.vk_buf = CreateBuffer(vctx, size, usage, mem_type_index);
    VULKAN_CALL(vkMapMemory(vctx.device, buf.vk_buf->memory, 0, size, 0, &(buf.host_addr)));
    buf.size = size;
  }
  return &buf;
}

VulkanStagingBuffer* VulkanThreadEntry::StagingBuffer(int device_id, size_t size) {
  const auto& vctx = VulkanDeviceAPI::Global()->context(device_id);
  auto usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  return GetOrAllocate(device_id, size, usage, vctx.staging_mtype_index, &staging_buffers_);
}

void VulkanThreadEntry::AllocateUniformBuffer(int device_id, size_t size) {
  const auto& vctx = VulkanDeviceAPI::Global()->context(device_id);
  auto prop = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  auto info = MakeBufferCreateInfo(vctx, size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  auto mem_type_index = FindMemoryType(vctx, info, prop);
  GetOrAllocate(device_id, size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, mem_type_index,
                &uniform_buffers_, true);
}

VulkanUniformBuffer* VulkanThreadEntry::GetUniformBuffer(int device_id, size_t size) {
  auto& buf = uniform_buffers_[device_id];
  ICHECK(buf);
  ICHECK_GE(buf->size, size);
  return buf.get();
}

VulkanThreadEntry::VulkanThreadEntry()
    : pool(std::make_unique<WorkspacePool>(static_cast<DLDeviceType>(kDLVulkan),
                                           VulkanDeviceAPI::Global())) {
  device.device_id = 0;
  device.device_type = static_cast<DLDeviceType>(kDLVulkan);
}

VulkanStream* VulkanThreadEntry::Stream(size_t device_id) {
  if (!streams_[device_id]) {
    streams_[device_id] = std::unique_ptr<VulkanStream>(
        new VulkanStream(&VulkanDeviceAPI::Global()->context(device_id)));
  }
  return streams_[device_id].get();
}

void VulkanWrappedFunc::operator()(TVMArgs args, TVMRetValue* rv,
                                   const ArgUnion64* pack_args) const {
  int device_id = VulkanThreadEntry::ThreadLocal()->device.device_id;
  ICHECK_LT(device_id, kVulkanMaxNumDevice);
  const auto& vctx = VulkanDeviceAPI::Global()->context(device_id);
  if (!scache_[device_id]) {
    scache_[device_id] = m_->GetPipeline(device_id, func_name_, num_pack_args_);
  }
  const auto& pipeline = scache_[device_id];
  ThreadWorkLoad wl = thread_axis_cfg_.Extract(args);
  std::vector<VkDescriptorBufferInfo> descriptor_buffers;
  descriptor_buffers.resize(num_buffer_args_);
  for (size_t i = 0; i < num_buffer_args_; ++i) {
    void* buf = args[static_cast<int>(i)];
    VkDescriptorBufferInfo binfo;
    binfo.buffer = static_cast<VulkanBuffer*>(buf)->buffer;
    binfo.offset = 0;
    binfo.range = VK_WHOLE_SIZE;
    descriptor_buffers[i] = binfo;
  }
  const size_t nbytes_scalars = num_pack_args_ * sizeof(ArgUnion64);
  if (pipeline->use_ubo) {
    auto ubo = VulkanThreadEntry::ThreadLocal()->GetUniformBuffer(device_id, nbytes_scalars);
    CHECK(ubo->host_addr) << "The UBO host buffer is not allocated";
    VkDescriptorBufferInfo binfo;
    binfo.buffer = ubo->vk_buf->buffer;
    binfo.offset = 0;
    binfo.range = VK_WHOLE_SIZE;
    descriptor_buffers.push_back(binfo);
  }
  if (vctx.UseImmediate()) {
    // Can safely capture by reference as this lambda is immediately executed on the calling thread.
    VulkanThreadEntry::ThreadLocal()->Stream(device_id)->Launch([&](VulkanStreamState* state) {
      vkCmdBindPipeline(state->cmd_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);
      ICHECK(pipeline->descriptor_update_template != VK_NULL_HANDLE);
      vctx.descriptor_template_khr_functions->vkCmdPushDescriptorSetWithTemplateKHR(
          state->cmd_buffer_, pipeline->descriptor_update_template, pipeline->pipeline_layout, 0,
          descriptor_buffers.data());

      if (pipeline->use_ubo) {
        auto ubo = VulkanThreadEntry::ThreadLocal()->GetUniformBuffer(device_id, nbytes_scalars);
        memcpy(ubo->host_addr, pack_args, nbytes_scalars);
      } else if (num_pack_args_ > 0) {
        vkCmdPushConstants(state->cmd_buffer_, pipeline->pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, num_pack_args_ * sizeof(ArgUnion64),
                           pack_args);
      }

      vkCmdDispatch(state->cmd_buffer_, wl.grid_dim(0), wl.grid_dim(1), wl.grid_dim(2));
      VkMemoryBarrier barrier_info;
      barrier_info.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      barrier_info.pNext = nullptr;
      barrier_info.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      barrier_info.dstAccessMask = (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
      vkCmdPipelineBarrier(state->cmd_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                           1, &barrier_info, 0, nullptr, 0, nullptr);
    });
    return;
  }

  // Otherwise, the more expensive deferred path.
  std::vector<ArgUnion64> pack_args_storage(pack_args, pack_args + num_pack_args_);
  const auto& deferred_initializer = [&vctx, pipeline, descriptor_buffers]() {
    std::vector<VkWriteDescriptorSet> write_descriptor_sets;
    write_descriptor_sets.resize(descriptor_buffers.size());
    for (size_t i = 0; i < write_descriptor_sets.size(); i++) {
      write_descriptor_sets[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_descriptor_sets[i].pNext = 0;
      write_descriptor_sets[i].dstSet = pipeline->descriptor_set;
      write_descriptor_sets[i].dstBinding = i;
      write_descriptor_sets[i].dstArrayElement = 0;
      write_descriptor_sets[i].descriptorCount = 1;
      write_descriptor_sets[i].pImageInfo = 0;
      write_descriptor_sets[i].pBufferInfo = &(descriptor_buffers[i]);
      write_descriptor_sets[i].pTexelBufferView = 0;

      if (pipeline->use_ubo && i == write_descriptor_sets.size() - 1) {
        // The last binding is for UBO
        write_descriptor_sets[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      } else {
        write_descriptor_sets[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      }
    }
    vkUpdateDescriptorSets(vctx.device, write_descriptor_sets.size(), write_descriptor_sets.data(),
                           0, 0);
  };
  const auto& deferred_kernel = [this, pipeline, wl, pack_args_storage, nbytes_scalars,
                                 device_id](VulkanStreamState* state) {
    vkCmdBindPipeline(state->cmd_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);
    vkCmdBindDescriptorSets(state->cmd_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline->pipeline_layout, 0, 1, &(pipeline->descriptor_set), 0,
                            nullptr);

    if (pipeline->use_ubo) {
      auto ubo = VulkanThreadEntry::ThreadLocal()->GetUniformBuffer(device_id, nbytes_scalars);
      memcpy(ubo->host_addr, pack_args_storage.data(), nbytes_scalars);
    } else if (num_pack_args_ > 0) {
      vkCmdPushConstants(state->cmd_buffer_, pipeline->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                         0, pack_args_storage.size() * sizeof(ArgUnion64),
                         pack_args_storage.data());
    }

    vkCmdDispatch(state->cmd_buffer_, wl.grid_dim(0), wl.grid_dim(1), wl.grid_dim(2));
    VkMemoryBarrier barrier_info;
    barrier_info.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier_info.pNext = nullptr;
    barrier_info.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier_info.dstAccessMask = (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdPipelineBarrier(state->cmd_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         1, &barrier_info, 0, nullptr, 0, nullptr);
  };
  VulkanStreamToken deferred_token;
  deferred_token.descriptor_set_ = pipeline->descriptor_set;
  deferred_token.buffers_.resize(descriptor_buffers.size());
  for (size_t i = 0; i < descriptor_buffers.size(); ++i) {
    deferred_token.buffers_[i] = descriptor_buffers[i].buffer;
  }
  VulkanThreadEntry::ThreadLocal()->Stream(device_id)->LaunchDeferred(
      deferred_initializer, deferred_kernel, deferred_token);
}

Module VulkanModuleLoadFile(const std::string& file_name, const std::string& format) {
  std::string data;
  std::unordered_map<std::string, VulkanShader> smap;
  std::unordered_map<std::string, FunctionInfo> fmap;
  std::string fmt = GetFileFormat(file_name, format);
  std::string meta_file = GetMetaFilePath(file_name);
  LoadBinaryFromFile(file_name, &data);
  LoadMetaDataFromFile(meta_file, &fmap);
  dmlc::MemoryStringStream fs(&data);
  dmlc::Stream* stream = &fs;
  uint32_t magic;
  stream->Read(&magic);
  ICHECK_EQ(magic, kVulkanModuleMagic) << "VulkanModule Magic mismatch";
  stream->Read(&smap);
  return VulkanModuleCreate(smap, fmap, "");
}

Module VulkanModuleLoadBinary(void* strm) {
  dmlc::Stream* stream = static_cast<dmlc::Stream*>(strm);
  std::unordered_map<std::string, VulkanShader> smap;
  std::unordered_map<std::string, FunctionInfo> fmap;

  std::string fmt;
  stream->Read(&fmt);
  stream->Read(&fmap);
  stream->Read(&smap);
  return VulkanModuleCreate(smap, fmap, "");
}

TVM_REGISTER_GLOBAL("runtime.module.loadfile_vulkan").set_body_typed(VulkanModuleLoadFile);

TVM_REGISTER_GLOBAL("runtime.module.loadbinary_vulkan").set_body_typed(VulkanModuleLoadBinary);

TVM_REGISTER_GLOBAL("device_api.vulkan").set_body([](TVMArgs args, TVMRetValue* rv) {
  DeviceAPI* ptr = VulkanDeviceAPI::Global();
  *rv = static_cast<void*>(ptr);
});

TVM_REGISTER_GLOBAL("device_api.vulkan.generate_target").set_body_typed([](int device_id) {
  return VulkanDeviceAPI::Global()->GenerateTarget(device_id);
});

}  // namespace vulkan
}  // namespace runtime
}  // namespace tvm
