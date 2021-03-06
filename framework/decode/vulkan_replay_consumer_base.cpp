/*
** Copyright (c) 2018-2019 Valve Corporation
** Copyright (c) 2018-2019 LunarG, Inc.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include "decode/vulkan_replay_consumer_base.h"

#include "decode/custom_vulkan_struct_handle_mappers.h"
#include "decode/descriptor_update_template_decoder.h"
#include "decode/vulkan_enum_util.h"
#include "generated/generated_vulkan_struct_handle_mappers.h"
#include "util/logging.h"
#include "util/platform.h"

#include <cstdint>
#include <limits>
#include <unordered_set>

GFXRECON_BEGIN_NAMESPACE(gfxrecon)
GFXRECON_BEGIN_NAMESPACE(decode)

const int32_t  kDefaultWindowPositionX = 0;
const int32_t  kDefaultWindowPositionY = 0;
const uint32_t kDefaultWindowWidth     = 320;
const uint32_t kDefaultWindowHeight    = 240;

const std::vector<std::string> kLoaderLibNames = {
#if defined(WIN32)
    "vulkan-1.dll"
#else
    "libvulkan.so", "libvulkan.so.1"
#endif
};

const std::unordered_set<std::string> kSurfaceExtensions = {
    VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, VK_MVK_IOS_SURFACE_EXTENSION_NAME, VK_MVK_MACOS_SURFACE_EXTENSION_NAME,
    VK_KHR_MIR_SURFACE_EXTENSION_NAME,     VK_NN_VI_SURFACE_EXTENSION_NAME,   VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    VK_KHR_WIN32_SURFACE_EXTENSION_NAME,   VK_KHR_XCB_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME
};

// Device extensions to enable for trimming state setup, when available.
const std::unordered_set<std::string> kTrimStateSetupDeviceExtensions = { VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME };

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugReportCallback(VkDebugReportFlagsEXT      flags,
                                                          VkDebugReportObjectTypeEXT objectType,
                                                          uint64_t                   object,
                                                          size_t                     location,
                                                          int32_t                    messageCode,
                                                          const char*                pLayerPrefix,
                                                          const char*                pMessage,
                                                          void*                      pUserData)
{
    GFXRECON_UNREFERENCED_PARAMETER(objectType);
    GFXRECON_UNREFERENCED_PARAMETER(object);
    GFXRECON_UNREFERENCED_PARAMETER(location);
    GFXRECON_UNREFERENCED_PARAMETER(messageCode);
    GFXRECON_UNREFERENCED_PARAMETER(pUserData);

    if ((pLayerPrefix != nullptr) && (pMessage != nullptr) &&
        ((flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) == VK_DEBUG_REPORT_ERROR_BIT_EXT))
    {
        GFXRECON_WRITE_CONSOLE("DEBUG REPORT: %s: %s", pLayerPrefix, pMessage);
    }

    return VK_FALSE;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
                                                         VkDebugUtilsMessageTypeFlagsEXT             messageTypes,
                                                         const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                         void*                                       pUserData)
{
    GFXRECON_UNREFERENCED_PARAMETER(pUserData);

    if ((pCallbackData != nullptr) && (pCallbackData->pMessageIdName != nullptr) &&
        (pCallbackData->pMessage != nullptr) &&
        ((messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) ==
         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) &&
        ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ==
         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))
    {
        GFXRECON_WRITE_CONSOLE("DEBUG MESSENGER: %s: %s", pCallbackData->pMessageIdName, pCallbackData->pMessage);
    }

    return VK_FALSE;
}

VulkanReplayConsumerBase::VulkanReplayConsumerBase(WindowFactory* window_factory, const ReplayOptions& options) :
    loader_handle_(nullptr), get_instance_proc_addr_(nullptr), create_instance_proc_(nullptr),
    window_factory_(window_factory), options_(options), loading_trim_state_(false)
{
    assert(window_factory != nullptr);
}

VulkanReplayConsumerBase::~VulkanReplayConsumerBase()
{
    // Destroy any windows that were created for Vulkan surfaces.
    for (const auto& entry : window_map_)
    {
        window_factory_->Destroy(entry.second);
    }

    if (loader_handle_ != nullptr)
    {
        util::platform::CloseLibrary(loader_handle_);
    }
}

void VulkanReplayConsumerBase::ProcessStateBeginMarker(uint64_t frame_number)
{
    GFXRECON_LOG_INFO("Loading state for captured frame %" PRId64, frame_number);
    loading_trim_state_ = true;
}

void VulkanReplayConsumerBase::ProcessStateEndMarker(uint64_t frame_number)
{
    GFXRECON_LOG_INFO("Finished loading state for captured frame %" PRId64, frame_number);
    loading_trim_state_ = false;
}

void VulkanReplayConsumerBase::ProcessDisplayMessageCommand(const std::string& message)
{
    GFXRECON_LOG_INFO("Trace Message: %s", message.c_str());
}

void VulkanReplayConsumerBase::ProcessFillMemoryCommand(uint64_t       memory_id,
                                                        uint64_t       offset,
                                                        uint64_t       size,
                                                        const uint8_t* data)
{
    // We need to find the device memory associated with this ID, and then lookup its mapped pointer.
    VkDeviceMemory memory = object_mapper_.MapVkDeviceMemory(memory_id);

    if (memory != VK_NULL_HANDLE)
    {
        auto entry = memory_map_.find(memory);

        if ((entry != memory_map_.end()) && (entry->second != nullptr))
        {
            GFXRECON_CHECK_CONVERSION_DATA_LOSS(size_t, size);
            memcpy(static_cast<uint8_t*>(entry->second) + offset, data, static_cast<size_t>(size));
        }
        else
        {
            GFXRECON_LOG_WARNING("Skipping memory fill for VkDeviceMemory object that is not mapped (%" PRIx64 ")",
                                 memory_id);
        }
    }
    else
    {
        GFXRECON_LOG_WARNING("Skipping memory fill for unrecognized VkDeviceMemory object (%" PRIx64 ")", memory_id);
    }
}

void VulkanReplayConsumerBase::ProcessResizeWindowCommand(format::HandleId surface_id, uint32_t width, uint32_t height)
{
    // We need to find the surface associated with this ID, and then lookup its window.
    VkSurfaceKHR surface = object_mapper_.MapVkSurfaceKHR(surface_id);

    if (surface != VK_NULL_HANDLE)
    {
        auto entry = window_map_.find(surface);

        if ((entry != window_map_.end()) && (entry->second != nullptr))
        {
            Window* window = entry->second;
            window->SetSize(width, height);
        }
        else
        {
            GFXRECON_LOG_WARNING(
                "Skipping window resize for VkSurface object (%" PRIx64 ") without an associated window", surface_id);
        }
    }
    else
    {
        GFXRECON_LOG_WARNING("Skipping window resize for unrecognized VkSurface object (%" PRIx64 ")", surface_id);
    }
}

void VulkanReplayConsumerBase::ProcessSetSwapchainImageStateCommand(
    format::HandleId                                    device_id,
    format::HandleId                                    swapchain_id,
    uint32_t                                            last_presented_image,
    const std::vector<format::SwapchainImageStateInfo>& image_info)
{
    VkDevice       device    = object_mapper_.MapVkDevice(device_id);
    VkSwapchainKHR swapchain = object_mapper_.MapVkSwapchainKHR(swapchain_id);

    if ((device != VK_NULL_HANDLE) && (swapchain != VK_NULL_HANDLE))
    {
        VkPhysicalDevice physical_device = device_parents_[device];
        VkSurfaceKHR     surface         = swapchain_surfaces_[swapchain];
        assert((physical_device != VK_NULL_HANDLE) && (surface != VK_NULL_HANDLE));

        auto instance_table = GetInstanceTable(physical_device);
        auto device_table   = GetDeviceTable(device);
        assert((instance_table != nullptr) && (device_table != nullptr));

        VkSurfaceCapabilitiesKHR surface_caps;
        uint32_t                 image_count = 0;

        VkResult result =
            instance_table->GetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &surface_caps);

        if (result == VK_SUCCESS)
        {
            result = device_table->GetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
        }

        if (result == VK_SUCCESS)
        {
            // TODO: Handle swapchain image count mismatch on replay.
            assert(image_info.size() == image_count);

            // Determine if it is possible to acquire all images at the same time.
            assert(image_count >= surface_caps.minImageCount);
            uint32_t max_acquired_images = (image_count - surface_caps.minImageCount) + 1;

            if (image_count > max_acquired_images)
            {
                // Cannot acquire all images at the same time.
                ProcessSetSwapchainImageStateQueueSubmit(device, swapchain, last_presented_image, image_info);
            }
            else
            {
                ProcessSetSwapchainImageStatePreAcquire(device, swapchain, image_info);
            }
        }
        else
        {
            GFXRECON_LOG_WARNING("Failed image initialization for VkSwapchainKHR object (ID = %" PRIu64
                                 ", handle = %" PRIx64 ")",
                                 swapchain_id,
                                 swapchain);
        }
    }
    else
    {
        if (device != VK_NULL_HANDLE)
        {
            GFXRECON_LOG_WARNING("Skipping image acquire for unrecognized VkSwapchainKHR object (ID = %" PRIu64 ")",
                                 swapchain_id);
        }
        else if (swapchain != VK_NULL_HANDLE)
        {
            GFXRECON_LOG_WARNING("Skipping image acquire for unrecognized VkDevice object (ID = %" PRIu64 ")",
                                 device_id);
        }
        else
        {
            GFXRECON_LOG_WARNING("Skipping image acquire for unrecognized VkDevice (ID = %" PRIu64
                                 ") and VkSwapchainKHR (ID = %" PRIu64 ") objects",
                                 device_id,
                                 swapchain_id);
        }
    }
}

void VulkanReplayConsumerBase::ProcessSetSwapchainImageStatePreAcquire(
    VkDevice device, VkSwapchainKHR swapchain, const std::vector<format::SwapchainImageStateInfo>& image_info)
{
    auto table = GetDeviceTable(device);
    assert(table != nullptr);

    VkResult        result             = VK_SUCCESS;
    VkQueue         transition_queue   = VK_NULL_HANDLE;
    VkCommandPool   transition_pool    = VK_NULL_HANDLE;
    VkCommandBuffer transition_command = VK_NULL_HANDLE;
    uint32_t        queue_family_index = swapchain_queue_families_[swapchain];

    // TODO: Improved queue selection?
    table->GetDeviceQueue(device, queue_family_index, 0, &transition_queue);

    VkCommandPoolCreateInfo pool_create_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool_create_info.pNext                   = nullptr;
    pool_create_info.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_create_info.queueFamilyIndex        = queue_family_index;

    result = table->CreateCommandPool(device, &pool_create_info, nullptr, &transition_pool);

    if (result == VK_SUCCESS)
    {
        VkCommandBufferAllocateInfo command_allocate_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        command_allocate_info.pNext                       = nullptr;
        command_allocate_info.commandBufferCount          = 1;
        command_allocate_info.commandPool                 = transition_pool;
        command_allocate_info.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

        table->AllocateCommandBuffers(device, &command_allocate_info, &transition_command);
    }

    if (result == VK_SUCCESS)
    {
        VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin_info.pNext                    = nullptr;
        begin_info.flags                    = 0;
        begin_info.pInheritanceInfo         = nullptr;

        VkSubmitInfo submit_info       = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit_info.pNext              = nullptr;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers    = &transition_command;

        VkImageMemoryBarrier image_barrier            = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        image_barrier.pNext                           = nullptr;
        image_barrier.srcAccessMask                   = 0;
        image_barrier.dstAccessMask                   = 0;
        image_barrier.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
        image_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        image_barrier.subresourceRange.baseMipLevel   = 0;
        image_barrier.subresourceRange.levelCount     = 1;
        image_barrier.subresourceRange.baseArrayLayer = 0;
        image_barrier.subresourceRange.layerCount     = 1;

        for (size_t i = 0; i < image_info.size(); ++i)
        {
            VkImage image = object_mapper_.MapVkImage(image_info[i].image_id);

            // Pre-acquire and transition swapchain images while processing trimming state snapshot.
            if (image != VK_NULL_HANDLE)
            {
                uint32_t image_index = 0;

                VkFence     acquire_fence     = VK_NULL_HANDLE;
                VkSemaphore acquire_semaphore = VK_NULL_HANDLE;

                VkFenceCreateInfo fence_create_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                fence_create_info.pNext             = nullptr;
                fence_create_info.flags             = 0;

                VkSemaphoreCreateInfo semaphore_create_info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
                semaphore_create_info.pNext                 = nullptr;
                semaphore_create_info.flags                 = 0;

                result = table->CreateFence(device, &fence_create_info, nullptr, &acquire_fence);

                if (result == VK_SUCCESS)
                {
                    result = table->CreateSemaphore(device, &semaphore_create_info, nullptr, &acquire_semaphore);
                }

                if (result == VK_SUCCESS)
                {
                    result = table->AcquireNextImageKHR(device,
                                                        swapchain,
                                                        std::numeric_limits<uint64_t>::max(),
                                                        acquire_semaphore,
                                                        acquire_fence,
                                                        &image_index);

                    if ((result == VK_SUCCESS) || (result == VK_SUBOPTIMAL_KHR))
                    {
                        // TODO: Handle case where image acquired at replay does not match image acquired at
                        // capture.
                        assert(image_index == i);

                        result =
                            table->WaitForFences(device, 1, &acquire_fence, true, std::numeric_limits<uint64_t>::max());

                        VkImageLayout image_layout = static_cast<VkImageLayout>(image_info[image_index].image_layout);
                        if ((result == VK_SUCCESS) && (image_layout != VK_IMAGE_LAYOUT_UNDEFINED))
                        {
                            image_barrier.newLayout = image_layout;
                            image_barrier.image     = image;

                            result = table->BeginCommandBuffer(transition_command, &begin_info);

                            if (result == VK_SUCCESS)
                            {
                                table->CmdPipelineBarrier(transition_command,
                                                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                                          0,
                                                          0,
                                                          nullptr,
                                                          0,
                                                          nullptr,
                                                          1,
                                                          &image_barrier);
                                table->EndCommandBuffer(transition_command);

                                result = table->ResetFences(device, 1, &acquire_fence);
                            }

                            if (result == VK_SUCCESS)
                            {
                                result = table->QueueSubmit(transition_queue, 1, &submit_info, acquire_fence);
                            }

                            if (result == VK_SUCCESS)
                            {
                                result = table->WaitForFences(
                                    device, 1, &acquire_fence, true, std::numeric_limits<uint64_t>::max());
                            }
                        }

                        if (result == VK_SUCCESS)
                        {
                            if (image_info[image_index].acquired)
                            {
                                // The upcoming frames expect the image to be acquired. The synchronization objects
                                // used to acquire the image were already set to the appropriate signaled state when
                                // created, so the temporary objects used to acquire the image here can be
                                // destroyed.
                                table->DestroyFence(device, acquire_fence, nullptr);
                                table->DestroySemaphore(device, acquire_semaphore, nullptr);
                            }
                            else
                            {
                                // The upcoming frames do not expect the image to be acquired. We will store the
                                // image and the synchronization objects used to acquire it in a data structure.
                                // Replay of vkAcquireNextImage will retrieve and use the stored objects.
                                swapchain_image_tracker_.TrackPreAcquiredImage(
                                    swapchain, image_index, acquire_semaphore, acquire_fence);
                            }
                        }
                        else
                        {
                            GFXRECON_LOG_WARNING("Failed to acquire and transition VkImage object (ID = %" PRIu64
                                                 ") for swapchain state initialization",
                                                 image_info[i].image_id);
                        }
                    }
                }
            }
            else
            {
                GFXRECON_LOG_WARNING("Skipping image acquire for unrecognized VkImage object (ID = %" PRIu64 ")",
                                     image_info[i].image_id);
            }
        }
    }
    else
    {
        GFXRECON_LOG_WARNING(
            "Failed to create image initialization resources for VkSwapchainKHR object (handle = 0x%" PRIx64 ")",
            swapchain);
    }

    if (transition_pool != VK_NULL_HANDLE)
    {
        table->DestroyCommandPool(device, transition_pool, nullptr);
    }
}

void VulkanReplayConsumerBase::ProcessSetSwapchainImageStateQueueSubmit(
    VkDevice                                            device,
    VkSwapchainKHR                                      swapchain,
    uint32_t                                            last_presented_image,
    const std::vector<format::SwapchainImageStateInfo>& image_info)
{
    auto table = GetDeviceTable(device);
    assert(table != nullptr);

    VkResult        result             = VK_SUCCESS;
    VkQueue         queue              = VK_NULL_HANDLE;
    VkCommandPool   pool               = VK_NULL_HANDLE;
    VkCommandBuffer command            = VK_NULL_HANDLE;
    VkFence         wait_fence         = VK_NULL_HANDLE;
    uint32_t        queue_family_index = swapchain_queue_families_[swapchain];

    VkCommandPoolCreateInfo pool_create_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool_create_info.pNext                   = nullptr;
    pool_create_info.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_create_info.queueFamilyIndex        = queue_family_index;

    // TODO: Improved queue selection?
    table->GetDeviceQueue(device, queue_family_index, 0, &queue);

    result = table->CreateCommandPool(device, &pool_create_info, nullptr, &pool);

    if (result == VK_SUCCESS)
    {
        VkCommandBufferAllocateInfo command_allocate_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        command_allocate_info.pNext                       = nullptr;
        command_allocate_info.commandBufferCount          = 1;
        command_allocate_info.commandPool                 = pool;
        command_allocate_info.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

        result = table->AllocateCommandBuffers(device, &command_allocate_info, &command);
    }

    if (result == VK_SUCCESS)
    {
        VkFenceCreateInfo fence_create_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fence_create_info.pNext             = nullptr;
        fence_create_info.flags             = VK_FENCE_CREATE_SIGNALED_BIT;

        result = table->CreateFence(device, &fence_create_info, nullptr, &wait_fence);
    }

    if (result == VK_SUCCESS)
    {
        VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin_info.pNext                    = nullptr;
        begin_info.flags                    = 0;
        begin_info.pInheritanceInfo         = nullptr;

        VkSubmitInfo submit_info       = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit_info.pNext              = nullptr;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers    = &command;

        VkImageMemoryBarrier image_barrier            = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        image_barrier.pNext                           = nullptr;
        image_barrier.srcAccessMask                   = 0;
        image_barrier.dstAccessMask                   = 0;
        image_barrier.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
        image_barrier.newLayout                       = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        image_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        image_barrier.subresourceRange.baseMipLevel   = 0;
        image_barrier.subresourceRange.levelCount     = 1;
        image_barrier.subresourceRange.baseArrayLayer = 0;
        image_barrier.subresourceRange.layerCount     = 1;

        VkPresentInfoKHR present_info   = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present_info.pNext              = nullptr;
        present_info.waitSemaphoreCount = 0;
        present_info.pWaitSemaphores    = nullptr;
        present_info.swapchainCount     = 1;
        present_info.pSwapchains        = &swapchain;
        present_info.pResults           = nullptr;

        // Acquire, transition to the present source layout, and present each image.
        for (size_t i = 0; i < image_info.size(); ++i)
        {
            VkImage image = object_mapper_.MapVkImage(image_info[i].image_id);

            if (image != VK_NULL_HANDLE)
            {
                uint32_t image_index = 0;

                result = table->ResetFences(device, 1, &wait_fence);

                if (result == VK_SUCCESS)
                {
                    result = table->AcquireNextImageKHR(device,
                                                        swapchain,
                                                        std::numeric_limits<uint64_t>::max(),
                                                        VK_NULL_HANDLE,
                                                        wait_fence,
                                                        &image_index);
                }

                if ((result == VK_SUCCESS) || (result == VK_SUBOPTIMAL_KHR))
                {
                    // TODO: Handle case where image acquired at replay does not match image acquired at capture.
                    assert(image_index == i);

                    result = table->WaitForFences(device, 1, &wait_fence, true, std::numeric_limits<uint64_t>::max());

                    if (result == VK_SUCCESS)
                    {
                        image_barrier.image        = image;
                        present_info.pImageIndices = &image_index;

                        result = table->BeginCommandBuffer(command, &begin_info);
                    }

                    if (result == VK_SUCCESS)
                    {
                        table->CmdPipelineBarrier(command,
                                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                                  0,
                                                  0,
                                                  nullptr,
                                                  0,
                                                  nullptr,
                                                  1,
                                                  &image_barrier);
                        table->EndCommandBuffer(command);

                        result = table->ResetFences(device, 1, &wait_fence);
                    }

                    if (result == VK_SUCCESS)
                    {
                        result = table->QueueSubmit(queue, 1, &submit_info, wait_fence);
                    }

                    if (result == VK_SUCCESS)
                    {
                        result =
                            table->WaitForFences(device, 1, &wait_fence, true, std::numeric_limits<uint64_t>::max());
                    }

                    if (result == VK_SUCCESS)
                    {
                        result = table->QueuePresentKHR(queue, &present_info);
                    }

                    if ((result == VK_SUCCESS) || (result == VK_SUBOPTIMAL_KHR))
                    {
                        result = table->QueueWaitIdle(queue);
                    }
                }

                if (result != VK_SUCCESS)
                {
                    GFXRECON_LOG_WARNING("Failed to acquire and transition VkImage object (ID = %" PRIu64
                                         ") for swapchain state initialization",
                                         image_info[i].image_id);
                }
            }
            else
            {
                GFXRECON_LOG_WARNING("Skipping image acquire for unrecognized VkImage object (ID = %" PRIu64 ")",
                                     image_info[i].image_id);
            }
        }

        // Second pass to set image acquired state.
        // Acquire all images up to the last presented image, to increase the chance that the first image
        // acquired on replay is the same image acquired by the first captured frame.
        for (size_t i = 0; i < image_info.size(); ++i)
        {
            VkImage image = object_mapper_.MapVkImage(image_info[i].image_id);

            if ((image != VK_NULL_HANDLE) && ((image_info[i].acquired) || (i <= last_presented_image)))
            {
                uint32_t image_index = 0;

                result = table->ResetFences(device, 1, &wait_fence);

                if (result == VK_SUCCESS)
                {
                    result = table->AcquireNextImageKHR(device,
                                                        swapchain,
                                                        std::numeric_limits<uint64_t>::max(),
                                                        VK_NULL_HANDLE,
                                                        wait_fence,
                                                        &image_index);
                }

                if ((result == VK_SUCCESS) || (result == VK_SUBOPTIMAL_KHR))
                {
                    // TODO: Handle case where image acquired at replay does not match image acquired at capture.
                    assert(image_index == i);

                    result = table->WaitForFences(device, 1, &wait_fence, true, std::numeric_limits<uint64_t>::max());

                    if (result == VK_SUCCESS)
                    {
                        if (image_info[i].acquired)
                        {
                            // Transition the image to the expected layout and keep it acquired.
                            VkImageLayout image_layout = static_cast<VkImageLayout>(image_info[i].image_layout);
                            if ((image_layout != VK_IMAGE_LAYOUT_UNDEFINED) &&
                                (image_layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR))
                            {

                                image_barrier.newLayout = image_layout;
                                image_barrier.image     = image;

                                result = table->BeginCommandBuffer(command, &begin_info);

                                if (result == VK_SUCCESS)
                                {
                                    table->CmdPipelineBarrier(command,
                                                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                                              0,
                                                              0,
                                                              nullptr,
                                                              0,
                                                              nullptr,
                                                              1,
                                                              &image_barrier);
                                    table->EndCommandBuffer(command);

                                    result = table->ResetFences(device, 1, &wait_fence);
                                }

                                if (result == VK_SUCCESS)
                                {
                                    result = table->QueueSubmit(queue, 1, &submit_info, wait_fence);
                                }

                                if (result == VK_SUCCESS)
                                {
                                    result = table->WaitForFences(
                                        device, 1, &wait_fence, true, std::numeric_limits<uint64_t>::max());
                                }
                            }
                        }
                        else
                        {
                            // Image is not expected to be in the acquired state, so present it.
                            present_info.pImageIndices = &image_index;

                            result = table->QueuePresentKHR(queue, &present_info);

                            if ((result == VK_SUCCESS) || (result == VK_SUBOPTIMAL_KHR))
                            {
                                result = table->QueueWaitIdle(queue);
                            }
                        }
                    }
                }

                if (result != VK_SUCCESS)
                {
                    GFXRECON_LOG_WARNING("Failed to acquire and transition VkImage object (ID = %" PRIu64
                                         ") for swapchain state initialization",
                                         image_info[i].image_id);
                }
            }
            else if (image == VK_NULL_HANDLE)
            {
                GFXRECON_LOG_WARNING("Skipping image acquire for unrecognized VkImage object (ID = %" PRIu64 ")",
                                     image_info[i].image_id);
            }
        }
    }
    else
    {
        GFXRECON_LOG_WARNING(
            "Failed to create image initialization resources for VkSwapchainKHR object (handle = 0x%" PRIx64 ")",
            swapchain);
    }

    if (pool != VK_NULL_HANDLE)
    {
        table->DestroyCommandPool(device, pool, nullptr);
    }

    if (wait_fence != VK_NULL_HANDLE)
    {
        table->DestroyFence(device, wait_fence, nullptr);
    }
}

void VulkanReplayConsumerBase::ProcessBeginResourceInitCommand(format::HandleId device_id,
                                                               uint64_t         max_resource_size,
                                                               uint64_t         max_copy_size)
{
    GFXRECON_UNREFERENCED_PARAMETER(max_resource_size);

    VkDevice device = object_mapper_.MapVkDevice(device_id);

    if (device != VK_NULL_HANDLE)
    {
        VkResult       result = VK_SUCCESS;
        VkBuffer       buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;

        auto table = GetDeviceTable(device);
        assert(table != nullptr);

        VkPhysicalDevice physical_device = device_parents_[device];
        assert(physical_device != VK_NULL_HANDLE);

        VkPhysicalDeviceMemoryProperties properties;
        auto                             instance_table = GetInstanceTable(physical_device);
        assert(instance_table != nullptr);

        instance_table->GetPhysicalDeviceMemoryProperties(physical_device, &properties);

        const auto& available_extensions      = trim_device_extensions_[physical_device];
        bool        have_shader_stencil_write = false;

        if (std::find(available_extensions.begin(),
                      available_extensions.end(),
                      VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME) != available_extensions.end())
        {
            have_shader_stencil_write = true;
        }

        resource_initializers_.emplace(device,
                                       std::make_unique<VulkanResourceInitializer>(
                                           device, max_copy_size, properties, have_shader_stencil_write, table));
    }
}

void VulkanReplayConsumerBase::ProcessEndResourceInitCommand(format::HandleId device_id)
{
    VkDevice device = object_mapper_.MapVkDevice(device_id);

    if (device != VK_NULL_HANDLE)
    {
        resource_initializers_.erase(device);
    }
}

void VulkanReplayConsumerBase::ProcessInitBufferCommand(format::HandleId device_id,
                                                        format::HandleId buffer_id,
                                                        uint64_t         data_size,
                                                        const uint8_t*   data)
{
    VkDevice device = object_mapper_.MapVkDevice(device_id);
    VkBuffer buffer = object_mapper_.MapVkBuffer(buffer_id);

    if ((device != VK_NULL_HANDLE) && (buffer != VK_NULL_HANDLE))
    {
        VkResult result = VK_SUCCESS;

        const BufferInfo& info = buffer_info_[buffer];

        VulkanResourceInitializer* initializer = nullptr;

        auto initializer_iter = resource_initializers_.find(device);
        if (initializer_iter != resource_initializers_.end())
        {
            initializer = initializer_iter->second.get();
        }

        if (initializer != nullptr)
        {
            if ((info.memory_property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ==
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            {
                assert(info.memory != VK_NULL_HANDLE);

                result = initializer->LoadData(info.memory, info.bind_offset, data_size, data);

                if (result != VK_SUCCESS)
                {
                    GFXRECON_LOG_WARNING("State snapshot mapped memory copy failed for VkBuffer object (ID = %" PRIu64
                                         ", handle = 0x%" PRIx64 ")",
                                         buffer_id,
                                         buffer);
                }
            }
            else
            {
                VkBufferCopy copy_region;
                copy_region.srcOffset = 0;
                copy_region.dstOffset = 0;
                copy_region.size      = data_size;

                result = initializer->InitializeBuffer(
                    data_size, data, info.queue_family_index, buffer, info.usage, 1, &copy_region);

                if (result != VK_SUCCESS)
                {
                    GFXRECON_LOG_WARNING("State snapshot staging buffer copy failed for VkBuffer object (ID = %" PRIu64
                                         ", handle = 0x%" PRIx64 ")",
                                         buffer_id,
                                         buffer);
                }
            }
        }
    }
    else
    {
        if (device != VK_NULL_HANDLE)
        {
            GFXRECON_LOG_WARNING(
                "Skipping state snapshot buffer upload for unrecognized VkBuffer object (ID = %" PRIu64 ")", buffer_id);
        }
        else if (buffer != VK_NULL_HANDLE)
        {
            GFXRECON_LOG_WARNING(
                "Skipping state snapshot buffer upload for unrecognized VkDevice object (ID = %" PRIu64 ")", device_id);
        }
        else
        {
            GFXRECON_LOG_WARNING("Skipping state snapshot buffer upload for unrecognized VkDevice (ID = %" PRIu64
                                 ") and VkBuffer (ID = %" PRIu64 ") objects",
                                 device_id,
                                 buffer_id);
        }
    }
}

void VulkanReplayConsumerBase::ProcessInitImageCommand(format::HandleId             device_id,
                                                       format::HandleId             image_id,
                                                       uint64_t                     data_size,
                                                       uint32_t                     aspect,
                                                       uint32_t                     layout,
                                                       const std::vector<uint64_t>& level_sizes,
                                                       const uint8_t*               data)
{
    VkDevice device = object_mapper_.MapVkDevice(device_id);
    VkImage  image  = object_mapper_.MapVkImage(image_id);

    if ((device != VK_NULL_HANDLE) && (image != VK_NULL_HANDLE))
    {
        VkResult                   result      = VK_SUCCESS;
        const ImageInfo&           info        = image_info_[image];
        VulkanResourceInitializer* initializer = nullptr;

        auto initializer_iter = resource_initializers_.find(device);
        if (initializer_iter != resource_initializers_.end())
        {
            initializer = initializer_iter->second.get();
        }

        if (initializer != nullptr)
        {
            std::vector<VkBufferImageCopy> copy_regions;

            if (data_size > 0)
            {
                if ((info.tiling == VK_IMAGE_TILING_LINEAR) &&
                    (info.memory_property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ==
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                {
                    assert(info.memory != VK_NULL_HANDLE);

                    result = initializer->LoadData(info.memory, info.bind_offset, data_size, data);

                    if (result != VK_SUCCESS)
                    {
                        GFXRECON_LOG_WARNING(
                            "State snapshot mapped memory copy failed for VkImage object (ID = %" PRIu64
                            ", handle = %" PRIx64 ")",
                            image_id,
                            image);
                    }
                }
                else
                {
                    // Create one copy region per mip-level.
                    VkBufferImageCopy copy_region;
                    copy_region.bufferRowLength                 = 0; // Request tightly packed data.
                    copy_region.bufferImageHeight               = 0; // Request tightly packed data.
                    copy_region.bufferOffset                    = 0;
                    copy_region.imageOffset.x                   = 0;
                    copy_region.imageOffset.y                   = 0;
                    copy_region.imageOffset.z                   = 0;
                    copy_region.imageSubresource.aspectMask     = aspect;
                    copy_region.imageSubresource.baseArrayLayer = 0;
                    copy_region.imageSubresource.layerCount     = info.layer_count;

                    assert(info.level_count == level_sizes.size());

                    for (uint32_t i = 0; i < info.level_count; ++i)
                    {
                        copy_region.imageSubresource.mipLevel = i;
                        copy_region.imageExtent.width         = std::max(1u, (info.extent.width >> i));
                        copy_region.imageExtent.height        = std::max(1u, (info.extent.height >> i));
                        copy_region.imageExtent.depth         = std::max(1u, (info.extent.depth >> i));

                        copy_regions.push_back(copy_region);
                        copy_region.bufferOffset += level_sizes[i];
                    }
                }
            }

            if (!copy_regions.empty())
            {
                result = initializer->InitializeImage(data_size,
                                                      data,
                                                      info.queue_family_index,
                                                      image,
                                                      info.type,
                                                      info.format,
                                                      info.extent,
                                                      static_cast<VkImageAspectFlagBits>(aspect),
                                                      info.sample_count,
                                                      info.usage,
                                                      static_cast<VkImageLayout>(info.initial_layout),
                                                      static_cast<VkImageLayout>(layout),
                                                      info.layer_count,
                                                      static_cast<uint32_t>(copy_regions.size()),
                                                      copy_regions.data());
            }
            else if ((layout != VK_IMAGE_LAYOUT_UNDEFINED) && (layout != VK_IMAGE_LAYOUT_PREINITIALIZED))
            {
                // Only transition to the final layout when a staging copy is not required for image data upload.
                result = initializer->TransitionImage(info.queue_family_index,
                                                      image,
                                                      info.format,
                                                      static_cast<VkImageAspectFlagBits>(aspect),
                                                      static_cast<VkImageLayout>(info.initial_layout),
                                                      static_cast<VkImageLayout>(layout),
                                                      info.layer_count,
                                                      info.level_count);
            }

            if (result != VK_SUCCESS)
            {
                GFXRECON_LOG_WARNING(
                    "State snapshot image upload/layout transition failed for VkImage object (ID = %" PRIu64
                    ", handle = %" PRIx64 ")",
                    image_id,
                    image);
            }
        }
    }
    else
    {
        if (device != VK_NULL_HANDLE)
        {
            GFXRECON_LOG_WARNING(
                "Skipping state snapshot image upload for unrecognized VkImage object (ID = %" PRIu64 ")", image_id);
        }
        else if (image != VK_NULL_HANDLE)
        {
            GFXRECON_LOG_WARNING(
                "Skipping state snapshot image upload for unrecognized VkDevice object (ID = %" PRIu64 ")", device_id);
        }
        else
        {
            GFXRECON_LOG_WARNING("Skipping state snapshot image upload for unrecognized VkDevice (ID = %" PRIu64
                                 ") and VkBuffer (ID = %" PRIu64 ") objects",
                                 device_id,
                                 image_id);
        }
    }
}

void VulkanReplayConsumerBase::RaiseFatalError(const char* message) const
{
    // TODO: Should there be a default action if no error handler has been provided?
    if (fatal_error_handler_ != nullptr)
    {
        fatal_error_handler_(message);
    }
}

void VulkanReplayConsumerBase::InitializeLoader()
{
    for (auto name : kLoaderLibNames)
    {
        loader_handle_ = util::platform::OpenLibrary(name.c_str());
        if (loader_handle_ != nullptr)
        {
            get_instance_proc_addr_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                util::platform::GetProcAddress(loader_handle_, "vkGetInstanceProcAddr"));
            break;
        }
    }

    if (get_instance_proc_addr_ != nullptr)
    {
        create_instance_proc_ =
            reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc_addr_(nullptr, "vkCreateInstance"));
    }

    if (create_instance_proc_ == nullptr)
    {
        GFXRECON_LOG_FATAL("Failed to load Vulkan runtime library; please ensure that the path to the Vulkan "
                           "loader (eg. %s) has been added to the appropriate system path",
                           kLoaderLibNames[0].c_str());
        RaiseFatalError("Failed to load Vulkan runtime library");
    }
}

void VulkanReplayConsumerBase::AddInstanceTable(VkInstance instance)
{
    encode::DispatchKey dispatch_key = encode::GetDispatchKey(instance);

    get_device_proc_addrs_[dispatch_key] =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_instance_proc_addr_(instance, "vkGetDeviceProcAddr"));
    create_device_procs_[dispatch_key] =
        reinterpret_cast<PFN_vkCreateDevice>(get_instance_proc_addr_(instance, "vkCreateDevice"));

    encode::InstanceTable& table = instance_tables_[dispatch_key];
    encode::LoadInstanceTable(get_instance_proc_addr_, instance, &table);
}

void VulkanReplayConsumerBase::AddDeviceTable(VkDevice device, PFN_vkGetDeviceProcAddr gpa)
{
    encode::DeviceTable& table = device_tables_[encode::GetDispatchKey(device)];
    encode::LoadDeviceTable(gpa, device, &table);
}

PFN_vkGetDeviceProcAddr VulkanReplayConsumerBase::GetDeviceAddrProc(VkPhysicalDevice physical_device)
{
    return get_device_proc_addrs_[encode::GetDispatchKey(physical_device)];
}

PFN_vkCreateDevice VulkanReplayConsumerBase::GetCreateDeviceProc(VkPhysicalDevice physical_device)
{
    return create_device_procs_[encode::GetDispatchKey(physical_device)];
}

const encode::InstanceTable* VulkanReplayConsumerBase::GetInstanceTable(const void* handle) const
{
    auto table = instance_tables_.find(encode::GetDispatchKey(handle));
    assert(table != instance_tables_.end());
    return (table != instance_tables_.end()) ? &table->second : nullptr;
}

const encode::DeviceTable* VulkanReplayConsumerBase::GetDeviceTable(const void* handle) const
{
    auto table = device_tables_.find(encode::GetDispatchKey(handle));
    assert(table != device_tables_.end());
    return (table != device_tables_.end()) ? &table->second : nullptr;
}

void* VulkanReplayConsumerBase::PreProcessExternalObject(uint64_t          object_id,
                                                         format::ApiCallId call_id,
                                                         const char*       call_name)
{
    void* object = nullptr;

    if ((call_id == format::ApiCallId::ApiCall_vkGetPhysicalDeviceWaylandPresentationSupportKHR) ||
        (call_id == format::ApiCallId::ApiCall_vkGetPhysicalDeviceXcbPresentationSupportKHR))
    {
        // For window system related handles, we put the object ID into the pointer.
        // The dispatch override for the API call will use this ID as a key to the window map.
        // TODO: For x86 builds, we should map the object_id to a 32-bit sequence number that won't be truncated by the
        // cast.
        object = reinterpret_cast<void*>(object_id);
    }
    else
    {
        GFXRECON_LOG_WARNING("Skipping object handle mapping for unsupported external object type processed by %s",
                             call_name);
    }

    return object;
}

void VulkanReplayConsumerBase::PostProcessExternalObject(const PointerDecoder<uint64_t>& object_id,
                                                         void*                           object,
                                                         format::ApiCallId               call_id,
                                                         const char*                     call_name)
{
    if (call_id == format::ApiCallId::ApiCall_vkMapMemory)
    {
        // Mapped memory tracking is handled by mapping the VkDeviceMemory handle to the mapped pointer, rather than
        // mapping the traced pointer address to the mapped pointer.  The memory needs to be tracked by handle so that
        // it can be removed from the unordered_map when memory is unmapped or freed.
    }
    else
    {
        GFXRECON_LOG_WARNING("Skipping object handle mapping for unsupported external object type processed by %s",
                             call_name);
    }
}

const VkAllocationCallbacks* VulkanReplayConsumerBase::GetAllocationCallbacks(
    const StructPointerDecoder<Decoded_VkAllocationCallbacks>& original_callbacks)
{
    // Replay does not currently attempt emulate the captured application's use of VkAllocationCallbacks.
    GFXRECON_UNREFERENCED_PARAMETER(original_callbacks);
    return nullptr;
}

void VulkanReplayConsumerBase::CheckResult(const char* func_name, VkResult original, VkResult replay)
{
    if (original != replay)
    {
        if ((replay < 0) && (replay != VK_ERROR_FORMAT_NOT_SUPPORTED))
        {
            // Raise a fatal error if replay produced an error that did not occur during capture.  Format not supported
            // errors are not treated as fatal, but will be reported as warnings below, allowing the replay to attempt
            // to continue for the case where an application may have queried for formats that it did not use.
            GFXRECON_LOG_FATAL("API call %s returned error value %s that does not match the result from the "
                               "capture file: %s.  Replay cannot continue.",
                               func_name,
                               enumutil::GetResultValueString(replay),
                               enumutil::GetResultValueString(original));

            RaiseFatalError(enumutil::GetResultDescription(replay));
        }
        else if (replay == VK_INCOMPLETE)
        {
            // VK_INCOMPLETE is generated when replaying a 'vkGet' function with a size value read from the capture file
            // that is smaller than the size expected by the replay device.  Replay does not make use of the values
            // retrieved by this function, so the error should be safe to ignore.  Log this case as a debug message
            // until replay is modified to adjust sizes used when replaying.
            GFXRECON_LOG_DEBUG("API call %s returned value VK_INCOMPLETE that does not match return value from "
                               "capture file: %s.  This may be caused by platform differences.",
                               func_name,
                               enumutil::GetResultValueString(original));
        }
        else if (!((replay == VK_SUCCESS) &&
                   ((original == VK_TIMEOUT) || (original == VK_NOT_READY) || (original == VK_ERROR_OUT_OF_DATE_KHR))))
        {
            // Report differences between replay result and capture result, unless the replay results indicates
            // that a wait operation completed before the original or a WSI function succeded when the original failed.
            GFXRECON_LOG_WARNING(
                "API call %s returned value %s that does not match return value from capture file: %s.",
                func_name,
                enumutil::GetResultValueString(replay),
                enumutil::GetResultValueString(original));
        }
    }
}

void VulkanReplayConsumerBase::ProcessPhysicalDeviceProperties(VkPhysicalDevice                  physical_device,
                                                               const VkPhysicalDeviceProperties* capture_properties,
                                                               const VkPhysicalDeviceProperties* replay_properties)
{
    if (device_properties_.find(physical_device) == device_properties_.end())
    {
        PhysicalDeviceProperties properties = { (*capture_properties), (*replay_properties) };
        device_properties_.emplace(physical_device, properties);
    }
}

void VulkanReplayConsumerBase::OverridePhysicalDevice(VkPhysicalDevice* physical_device)
{
    assert(physical_device != nullptr);

    // Match the current physical device with its parent instance.
    VkInstance instance = VK_NULL_HANDLE;

    for (const auto& entry : instance_devices_)
    {
        auto result =
            std::find(entry.second.replay_devices.begin(), entry.second.replay_devices.end(), (*physical_device));
        if (result != entry.second.replay_devices.end())
        {
            instance = entry.first;
            break;
        }
    }

    if (instance != VK_NULL_HANDLE)
    {
        auto devices_entry = instance_devices_.find(instance);
        if (devices_entry != instance_devices_.end())
        {
            const auto& replay_devices = devices_entry->second.replay_devices;
            if (options_.override_gpu_index < replay_devices.size())
            {
                VkPhysicalDevice override_device = replay_devices[options_.override_gpu_index];

                if (override_device != (*physical_device))
                {
                    GFXRECON_LOG_INFO("Overriding replay device with GPU%d", options_.override_gpu_index);
                    GFXRECON_LOG_INFO("  Available devices are:");
                    for (size_t i = 0; i < replay_devices.size(); ++i)
                    {
                        GFXRECON_LOG_INFO("    [%d] %p", i, replay_devices[i]);
                    }
                    GFXRECON_LOG_INFO("  Original device is: %p", *physical_device);
                    GFXRECON_LOG_INFO("  Override device is: %p", override_device);

                    // Warn about potential incompatibilities when replay device type does not match capture
                    // device type.
                    auto capture_entry = device_properties_.find(*physical_device);
                    auto replay_entry  = device_properties_.find(override_device);
                    if ((capture_entry != device_properties_.end()) && (replay_entry != device_properties_.end()))
                    {
                        const auto& capture_properties = capture_entry->second.capture_properties;
                        const auto& replay_properties  = replay_entry->second.replay_properties;
                        if ((capture_properties.vendorID != replay_properties.vendorID) &&
                            (capture_properties.deviceID != replay_properties.deviceID))
                        {
                            GFXRECON_LOG_WARNING(
                                "The type of device selected for replay differs from the type of the original capture "
                                "device; replay may fail due to device incompatibilities:");
                            GFXRECON_LOG_WARNING(
                                "  Capture device info:\t[vendorID = 0x%x, deviceId = 0x%x, deviceName = %s]",
                                capture_properties.vendorID,
                                capture_properties.deviceID,
                                capture_properties.deviceName);
                            GFXRECON_LOG_WARNING(
                                "  Replay device info:\t[vendorID = 0x%x, deviceId = 0x%x, deviceName = %s]",
                                replay_properties.vendorID,
                                replay_properties.deviceID,
                                replay_properties.deviceName);
                        }
                    }
                    else
                    {
                        // No query was made for physical device properties prior to device creation at capture,
                        // so provide a generic warning.
                        GFXRECON_LOG_WARNING(
                            "If the type of device selected for replay differs from the type of the original capture "
                            "device, replay may fail due to device incompatibilities.");
                    }

                    (*physical_device) = override_device;
                }
            }
            else
            {
                GFXRECON_LOG_ERROR("Index specified for replay device override (%d) exceeds the range of "
                                   "available physical devices (0-%d); the override will not be applied",
                                   options_.override_gpu_index,
                                   (replay_devices.size() - 1));
            }
        }
        else
        {
            GFXRECON_LOG_ERROR("Replay device override failed to find a list of physical devices associated with "
                               "the current instance; the override will not be applied")
        }
    }
    else
    {
        GFXRECON_LOG_ERROR("Replay device override failed to match the original physical device handle with an "
                           "instance; the override will not be applied")
    }
}

bool VulkanReplayConsumerBase::CheckTrimDeviceExtensions(VkPhysicalDevice           physical_device,
                                                         std::vector<std::string>** extensions)
{
    bool have_extensions = false;
    auto table           = GetInstanceTable(physical_device);
    assert(table != nullptr);

    uint32_t count  = 0;
    VkResult result = table->EnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr);

    if ((result == VK_SUCCESS) && (count > 0))
    {
        std::vector<VkExtensionProperties> properties;
        properties.resize(count);

        result = table->EnumerateDeviceExtensionProperties(physical_device, nullptr, &count, properties.data());

        if (result == VK_SUCCESS)
        {
            assert(count == properties.size());

            auto& entry = trim_device_extensions_[physical_device];

            for (const auto& property : properties)
            {
                if (kTrimStateSetupDeviceExtensions.find(property.extensionName) !=
                    kTrimStateSetupDeviceExtensions.end())
                {
                    entry.push_back(property.extensionName);
                }
            }

            have_extensions = !entry.empty();

            if (extensions != nullptr)
            {
                (*extensions) = &entry;
            }
        }
    }

    return have_extensions;
}

VkResult VulkanReplayConsumerBase::CreateSurface(VkInstance instance, VkFlags flags, VkSurfaceKHR* surface)
{
    // Create a window for our surface.
    Window* window = window_factory_->Create(
        kDefaultWindowPositionX, kDefaultWindowPositionY, kDefaultWindowWidth, kDefaultWindowHeight);

    if (window == nullptr)
    {
        // Failure to create a window is a fatal error.
        GFXRECON_LOG_FATAL("Failed to create a window for use with surface creation.  Replay cannot continue.");
        RaiseFatalError("Replay has encountered a fatal error and cannot continue (window creation failed)");
    }

    VkResult result = window->CreateSurface(GetInstanceTable(instance), instance, flags, surface);

    if ((result == VK_SUCCESS) && (surface != nullptr))
    {
        VkSurfaceKHR key = (*surface);

        assert(window_map_.find(key) == window_map_.end());

        window_map_.insert(std::make_pair(key, window));
    }
    else
    {
        window_factory_->Destroy(window);
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideCreateInstance(VkResult                                original_result,
                                                          const VkInstanceCreateInfo*             pCreateInfo,
                                                          const VkAllocationCallbacks*            pAllocator,
                                                          const HandlePointerDecoder<VkInstance>& original_instance,
                                                          VkInstance*                             pInstance)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(original_instance);

    if (loader_handle_ == nullptr)
    {
        InitializeLoader();
    }

    // Replace WSI extension in extension list.
    std::vector<const char*> filtered_extensions;
    VkInstanceCreateInfo     modified_create_info{};

    if (pCreateInfo != nullptr)
    {
        if (pCreateInfo->ppEnabledExtensionNames)
        {
            for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
            {
                const char* current_extension = pCreateInfo->ppEnabledExtensionNames[i];
                if (kSurfaceExtensions.find(current_extension) != kSurfaceExtensions.end())
                {
                    filtered_extensions.push_back(window_factory_->GetSurfaceExtensionName());
                }
                else
                {
                    filtered_extensions.push_back(current_extension);
                }
            }
        }

        modified_create_info                         = (*pCreateInfo);
        modified_create_info.enabledExtensionCount   = static_cast<uint32_t>(filtered_extensions.size());
        modified_create_info.ppEnabledExtensionNames = filtered_extensions.data();
    }
    else
    {
        GFXRECON_LOG_WARNING("The vkCreateInstance parameter pCreateInfo is NULL.");
    }

    // Disable layers; any layers needed for replay should be enabled for the replay app with the VK_INSTANCE_LAYERS
    // environment variable or debug.vulkan.layers Android property.
    if (modified_create_info.enabledLayerCount > 0)
    {
        GFXRECON_LOG_INFO(
            "Replay has removed the following layers from VkInstanceCreateInfo when calling vkCreateInstance:");

        for (uint32_t i = 0; i < modified_create_info.enabledLayerCount; ++i)
        {
            GFXRECON_LOG_INFO("\t%s", modified_create_info.ppEnabledLayerNames[i]);
        }

        modified_create_info.enabledLayerCount   = 0;
        modified_create_info.ppEnabledLayerNames = nullptr;
    }

    VkResult result = create_instance_proc_(&modified_create_info, pAllocator, pInstance);

    if ((pInstance != nullptr) && (result == VK_SUCCESS))
    {
        AddInstanceTable(*pInstance);
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideCreateDevice(VkResult                              original_result,
                                                        VkPhysicalDevice                      physicalDevice,
                                                        const VkDeviceCreateInfo*             pCreateInfo,
                                                        const VkAllocationCallbacks*          pAllocator,
                                                        const HandlePointerDecoder<VkDevice>& original_device,
                                                        VkDevice*                             pDevice)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(original_device);

    VkResult                result               = VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetDeviceProcAddr get_device_proc_addr = GetDeviceAddrProc(physicalDevice);
    PFN_vkCreateDevice      create_device_proc   = GetCreateDeviceProc(physicalDevice);

    if ((get_device_proc_addr != nullptr) && (create_device_proc != nullptr))
    {
        if (options_.override_gpu_index >= 0)
        {
            OverridePhysicalDevice(&physicalDevice);
        }

        std::vector<std::string>* extensions = nullptr;
        if (loading_trim_state_ && CheckTrimDeviceExtensions(physicalDevice, &extensions))
        {
            std::vector<const char*> modified_extensions;
            VkDeviceCreateInfo       modified_create_info{};

            if (pCreateInfo != nullptr)
            {
                if (pCreateInfo->ppEnabledExtensionNames)
                {
                    modified_extensions.insert(
                        modified_extensions.begin(),
                        pCreateInfo->ppEnabledExtensionNames,
                        (pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount));
                }

                for (const auto& extension : *extensions)
                {
                    if (std::find(modified_extensions.begin(), modified_extensions.end(), extension) ==
                        modified_extensions.end())
                    {
                        modified_extensions.push_back(extension.c_str());
                    }
                }

                modified_create_info                         = (*pCreateInfo);
                modified_create_info.enabledExtensionCount   = static_cast<uint32_t>(modified_extensions.size());
                modified_create_info.ppEnabledExtensionNames = modified_extensions.data();
            }
            else
            {
                GFXRECON_LOG_WARNING("The vkCreateDevice parameter pCreateInfo is NULL.");
            }

            result = create_device_proc(physicalDevice, &modified_create_info, pAllocator, pDevice);
        }
        else
        {
            result = create_device_proc(physicalDevice, pCreateInfo, pAllocator, pDevice);
        }

        if ((pDevice != nullptr) && (result == VK_SUCCESS))
        {
            AddDeviceTable(*pDevice, get_device_proc_addr);
            device_parents_[*pDevice] = physicalDevice;
        }
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideEnumeratePhysicalDevices(
    PFN_vkEnumeratePhysicalDevices                func,
    VkResult                                      returnValue,
    VkInstance                                    instance,
    const PointerDecoder<uint32_t>&               original_device_count,
    uint32_t*                                     pPhysicalDeviceCount,
    const HandlePointerDecoder<VkPhysicalDevice>& original_devices,
    VkPhysicalDevice*                             pPhysicalDevices)
{
    GFXRECON_UNREFERENCED_PARAMETER(returnValue);

    VkResult result = func(instance, pPhysicalDeviceCount, pPhysicalDevices);

    if ((result >= 0) && (pPhysicalDevices != nullptr) && (instance_devices_.find(instance) == instance_devices_.end()))
    {
        assert(!original_devices.IsNull() && !original_device_count.IsNull() && (pPhysicalDeviceCount != nullptr));

        InstanceDevices   devices;
        format::HandleId* capture_devices = original_devices.GetPointer();
        uint32_t          count           = (*original_device_count.GetPointer());

        for (uint32_t i = 0; i < count; ++i)
        {
            devices.capture_devices.push_back(capture_devices[i]);
        }

        count = (*pPhysicalDeviceCount);

        for (uint32_t i = 0; i < count; ++i)
        {
            devices.replay_devices.push_back(pPhysicalDevices[i]);
        }

        instance_devices_.emplace(instance, devices);
    }

    return result;
}

void VulkanReplayConsumerBase::OverrideGetPhysicalDeviceProperties(
    PFN_vkGetPhysicalDeviceProperties                               func,
    VkPhysicalDevice                                                physicalDevice,
    const StructPointerDecoder<Decoded_VkPhysicalDeviceProperties>& original_properties,
    VkPhysicalDeviceProperties*                                     pProperties)
{
    func(physicalDevice, pProperties);
    ProcessPhysicalDeviceProperties(physicalDevice, original_properties.GetPointer(), pProperties);
}

void VulkanReplayConsumerBase::OverrideGetPhysicalDeviceProperties2(
    PFN_vkGetPhysicalDeviceProperties2                               func,
    VkPhysicalDevice                                                 physicalDevice,
    const StructPointerDecoder<Decoded_VkPhysicalDeviceProperties2>& original_properties,
    VkPhysicalDeviceProperties2*                                     pProperties)
{
    func(physicalDevice, pProperties);

    auto properties = original_properties.GetPointer();
    assert((properties != nullptr) && (pProperties != nullptr));
    ProcessPhysicalDeviceProperties(physicalDevice, &properties->properties, &pProperties->properties);
}

void VulkanReplayConsumerBase::OverrideGetPhysicalDeviceProperties2KHR(
    PFN_vkGetPhysicalDeviceProperties2KHR                               func,
    VkPhysicalDevice                                                    physicalDevice,
    const StructPointerDecoder<Decoded_VkPhysicalDeviceProperties2KHR>& original_properties,
    VkPhysicalDeviceProperties2KHR*                                     pProperties)
{
    func(physicalDevice, pProperties);

    auto properties = original_properties.GetPointer();
    assert((properties != nullptr) && (pProperties != nullptr));
    ProcessPhysicalDeviceProperties(physicalDevice, &properties->properties, &pProperties->properties);
}

VkResult VulkanReplayConsumerBase::OverrideWaitForFences(PFN_vkWaitForFences func,
                                                         VkResult            original_result,
                                                         VkDevice            device,
                                                         uint32_t            fenceCount,
                                                         const VkFence*      pFences,
                                                         VkBool32            waitAll,
                                                         uint64_t            timeout)
{
    VkResult result;

    if (original_result == VK_SUCCESS)
    {
        // Ensure that wait for fences waits until the fences have been signaled (or error occurs) by changing the
        // timeout to UINT64_MAX.
        result = func(device, fenceCount, pFences, waitAll, std::numeric_limits<uint64_t>::max());
    }
    else if (original_result == VK_TIMEOUT)
    {
        // Try to get a timeout result with a 0 timeout.
        result = func(device, fenceCount, pFences, waitAll, 0);
    }
    else
    {
        result = func(device, fenceCount, pFences, waitAll, timeout);
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideGetFenceStatus(PFN_vkGetFenceStatus func,
                                                          VkResult             original_result,
                                                          VkDevice             device,
                                                          VkFence              fence)
{
    VkResult result;

    do
    {
        result = func(device, fence);
    } while ((original_result == VK_SUCCESS) && (result == VK_NOT_READY));

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideGetEventStatus(PFN_vkGetEventStatus func,
                                                          VkResult             original_result,
                                                          VkDevice             device,
                                                          VkEvent              event)
{
    VkResult result;

    do
    {
        result = func(device, event);
    } while ((original_result == VK_EVENT_SET) && (result == VK_EVENT_RESET));

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideGetQueryPoolResults(PFN_vkGetQueryPoolResults      func,
                                                               VkResult                       original_result,
                                                               VkDevice                       device,
                                                               VkQueryPool                    queryPool,
                                                               uint32_t                       firstQuery,
                                                               uint32_t                       queryCount,
                                                               size_t                         dataSize,
                                                               const PointerDecoder<uint8_t>& original_data,
                                                               void*                          pData,
                                                               VkDeviceSize                   stride,
                                                               VkQueryResultFlags             flags)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_data);

    VkResult result;

    do
    {
        result = func(device, queryPool, firstQuery, queryCount, dataSize, pData, stride, flags);
    } while ((original_result == VK_SUCCESS) && (result == VK_NOT_READY));

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideAllocateCommandBuffers(
    PFN_vkAllocateCommandBuffers                 func,
    VkResult                                     original_result,
    VkDevice                                     device,
    const VkCommandBufferAllocateInfo*           pAllocateInfo,
    const HandlePointerDecoder<VkCommandBuffer>& original_command_buffers,
    VkCommandBuffer*                             pCommandBuffers)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_command_buffers);

    VkResult result = original_result;

    if ((original_result >= 0) || !options_.skip_failed_allocations)
    {
        result = func(device, pAllocateInfo, pCommandBuffers);
    }
    else
    {
        GFXRECON_LOG_INFO("Skipping vkAllocateCommandBuffers call that failed during capture with error %s",
                          enumutil::GetResultValueString(original_result));
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideAllocateDescriptorSets(
    PFN_vkAllocateDescriptorSets                 func,
    VkResult                                     original_result,
    VkDevice                                     device,
    const VkDescriptorSetAllocateInfo*           pAllocateInfo,
    const HandlePointerDecoder<VkDescriptorSet>& original_descriptor_sets,
    VkDescriptorSet*                             pDescriptorSets)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_descriptor_sets);

    VkResult result = original_result;

    if ((original_result >= 0) || !options_.skip_failed_allocations)
    {
        result = func(device, pAllocateInfo, pDescriptorSets);
    }
    else
    {
        GFXRECON_LOG_INFO("Skipping vkAllocateDescriptorSets call that failed during capture with error %s",
                          enumutil::GetResultValueString(original_result));
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideAllocateMemory(PFN_vkAllocateMemory                        func,
                                                          VkResult                                    original_result,
                                                          VkDevice                                    device,
                                                          const VkMemoryAllocateInfo*                 pAllocateInfo,
                                                          const VkAllocationCallbacks*                pAllocator,
                                                          const HandlePointerDecoder<VkDeviceMemory>& original_memory,
                                                          VkDeviceMemory*                             pMemory)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_memory);

    VkResult result = original_result;

    if ((original_result >= 0) || !options_.skip_failed_allocations)
    {
        result = func(device, pAllocateInfo, pAllocator, pMemory);

        if ((result == VK_SUCCESS) && (pAllocateInfo != nullptr) && (pMemory != nullptr))
        {
            VkPhysicalDevice physical_device = device_parents_[device];
            assert(physical_device != VK_NULL_HANDLE);

            auto table = GetInstanceTable(physical_device);
            assert(table != nullptr);

            // TODO: Store memory properties with physical device handle to avoid multiple property queries.
            VkPhysicalDeviceMemoryProperties properties;
            table->GetPhysicalDeviceMemoryProperties(physical_device, &properties);

            assert(pAllocateInfo->memoryTypeIndex < properties.memoryTypeCount);
            memory_properties_[*pMemory] = properties.memoryTypes[pAllocateInfo->memoryTypeIndex].propertyFlags;
        }
    }
    else
    {
        GFXRECON_LOG_INFO("Skipping vkAllocateMemory call that failed during capture with error %s",
                          enumutil::GetResultValueString(original_result));
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideMapMemory(PFN_vkMapMemory                 func,
                                                     VkResult                        original_result,
                                                     VkDevice                        device,
                                                     VkDeviceMemory                  memory,
                                                     VkDeviceSize                    offset,
                                                     VkDeviceSize                    size,
                                                     VkMemoryMapFlags                flags,
                                                     const PointerDecoder<uint64_t>& original_data,
                                                     void**                          ppData)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(original_data);

    VkResult result = func(device, memory, offset, size, flags, ppData);

    if ((result == VK_SUCCESS) && (ppData != nullptr) && (*ppData != nullptr))
    {
        memory_map_.insert(std::make_pair(memory, (*ppData)));
    }

    return result;
}

void VulkanReplayConsumerBase::OverrideUnmapMemory(PFN_vkUnmapMemory func, VkDevice device, VkDeviceMemory memory)
{
    memory_map_.erase(memory);

    func(device, memory);
}

void VulkanReplayConsumerBase::OverrideFreeMemory(PFN_vkFreeMemory             func,
                                                  VkDevice                     device,
                                                  VkDeviceMemory               memory,
                                                  const VkAllocationCallbacks* pAllocator)
{
    memory_map_.erase(memory);

    func(device, memory, pAllocator);
}

VkResult VulkanReplayConsumerBase::OverrideBindBufferMemory(PFN_vkBindBufferMemory func,
                                                            VkResult               original_result,
                                                            VkDevice               device,
                                                            VkBuffer               buffer,
                                                            VkDeviceMemory         memory,
                                                            VkDeviceSize           memoryOffset)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);

    VkResult result = func(device, buffer, memory, memoryOffset);

    if (result == VK_SUCCESS)
    {
        BufferInfo& info           = buffer_info_[buffer];
        info.memory                = memory;
        info.memory_property_flags = memory_properties_[memory];
        info.bind_offset           = memoryOffset;
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideBindBufferMemory2(PFN_vkBindBufferMemory2       func,
                                                             VkResult                      original_result,
                                                             VkDevice                      device,
                                                             uint32_t                      bindInfoCount,
                                                             const VkBindBufferMemoryInfo* pBindInfos)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);

    VkResult result = func(device, bindInfoCount, pBindInfos);

    if ((result == VK_SUCCESS) && (pBindInfos != nullptr))
    {
        for (uint32_t i = 0; i < bindInfoCount; ++i)
        {
            const VkBindBufferMemoryInfo* bind_info = &pBindInfos[i];

            BufferInfo& info           = buffer_info_[bind_info->buffer];
            info.memory                = bind_info->memory;
            info.memory_property_flags = memory_properties_[bind_info->memory];
            info.bind_offset           = bind_info->memoryOffset;
        }
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideBindImageMemory(PFN_vkBindImageMemory func,
                                                           VkResult              original_result,
                                                           VkDevice              device,
                                                           VkImage               image,
                                                           VkDeviceMemory        memory,
                                                           VkDeviceSize          memoryOffset)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);

    VkResult result = func(device, image, memory, memoryOffset);

    if (result == VK_SUCCESS)
    {
        ImageInfo& info            = image_info_[image];
        info.memory                = memory;
        info.memory_property_flags = memory_properties_[memory];
        info.bind_offset           = memoryOffset;
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideBindImageMemory2(PFN_vkBindImageMemory2       func,
                                                            VkResult                     original_result,
                                                            VkDevice                     device,
                                                            uint32_t                     bindInfoCount,
                                                            const VkBindImageMemoryInfo* pBindInfos)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);

    VkResult result = func(device, bindInfoCount, pBindInfos);

    if ((result == VK_SUCCESS) && (pBindInfos != nullptr))
    {
        for (uint32_t i = 0; i < bindInfoCount; ++i)
        {
            const VkBindImageMemoryInfo* bind_info = &pBindInfos[i];

            ImageInfo& info            = image_info_[bind_info->image];
            info.memory                = bind_info->memory;
            info.memory_property_flags = memory_properties_[bind_info->memory];
            info.bind_offset           = bind_info->memoryOffset;
        }
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideCreateBuffer(PFN_vkCreateBuffer                    func,
                                                        VkResult                              original_result,
                                                        VkDevice                              device,
                                                        const VkBufferCreateInfo*             pCreateInfo,
                                                        const VkAllocationCallbacks*          pAllocator,
                                                        const HandlePointerDecoder<VkBuffer>& original_buffer,
                                                        VkBuffer*                             pBuffer)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(original_buffer);

    VkResult result = func(device, pCreateInfo, pAllocator, pBuffer);

    if ((result == VK_SUCCESS) && (pCreateInfo != nullptr) && (pBuffer != nullptr))
    {
        BufferInfo& info = buffer_info_[*pBuffer];
        info.usage       = pCreateInfo->usage;

        if ((pCreateInfo->sharingMode == VK_SHARING_MODE_CONCURRENT) && (pCreateInfo->queueFamilyIndexCount > 0) &&
            (pCreateInfo->pQueueFamilyIndices != nullptr))
        {
            info.queue_family_index = pCreateInfo->pQueueFamilyIndices[0];
        }
        else
        {
            info.queue_family_index = 0;
        }
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideCreateImage(PFN_vkCreateImage                    func,
                                                       VkResult                             original_result,
                                                       VkDevice                             device,
                                                       const VkImageCreateInfo*             pCreateInfo,
                                                       const VkAllocationCallbacks*         pAllocator,
                                                       const HandlePointerDecoder<VkImage>& original_image,
                                                       VkImage*                             pImage)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(original_image);

    VkResult result = func(device, pCreateInfo, pAllocator, pImage);

    if ((result == VK_SUCCESS) && (pCreateInfo != nullptr) && (pImage != nullptr))
    {
        ImageInfo& info     = image_info_[*pImage];
        info.usage          = pCreateInfo->usage;
        info.type           = pCreateInfo->imageType;
        info.format         = pCreateInfo->format;
        info.extent         = pCreateInfo->extent;
        info.tiling         = pCreateInfo->tiling;
        info.sample_count   = pCreateInfo->samples;
        info.initial_layout = pCreateInfo->initialLayout;
        info.layer_count    = pCreateInfo->arrayLayers;
        info.level_count    = pCreateInfo->mipLevels;

        if ((pCreateInfo->sharingMode == VK_SHARING_MODE_CONCURRENT) && (pCreateInfo->queueFamilyIndexCount > 0) &&
            (pCreateInfo->pQueueFamilyIndices != nullptr))
        {
            info.queue_family_index = pCreateInfo->pQueueFamilyIndices[0];
        }
        else
        {
            info.queue_family_index = 0;
        }
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideCreateDescriptorUpdateTemplate(
    PFN_vkCreateDescriptorUpdateTemplate                    func,
    VkResult                                                original_result,
    VkDevice                                                device,
    const VkDescriptorUpdateTemplateCreateInfo*             pCreateInfo,
    const VkAllocationCallbacks*                            pAllocator,
    const HandlePointerDecoder<VkDescriptorUpdateTemplate>& original_update_template,
    VkDescriptorUpdateTemplate*                             pDescriptorUpdateTemplate)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(original_update_template);

    if (pCreateInfo != nullptr)
    {
        // Modify the layout of the update template entries to match the tight packing performed by the trace encoding.
        // The trace encoding wrote the update template entries as a tightly packed array of VkDescriptorImageInfo
        // values, followed by an array of VkDescriptorBufferInfo values, followed by an array of VkBufferView values.
        VkDescriptorUpdateTemplateCreateInfo override_create_info = (*pCreateInfo);

        std::vector<VkDescriptorUpdateTemplateEntry> entries(
            override_create_info.pDescriptorUpdateEntries,
            (override_create_info.pDescriptorUpdateEntries + override_create_info.descriptorUpdateEntryCount));

        // Count the number of values of each type.
        size_t image_info_count        = 0;
        size_t buffer_info_count       = 0;
        size_t texel_buffer_view_count = 0;

        for (auto entry = entries.begin(); entry != entries.end(); ++entry)
        {
            VkDescriptorType type = entry->descriptorType;

            if ((type == VK_DESCRIPTOR_TYPE_SAMPLER) || (type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ||
                (type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) || (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ||
                (type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT))
            {
                image_info_count += entry->descriptorCount;
            }
            else if ((type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) || (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) ||
                     (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) ||
                     (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC))
            {
                buffer_info_count += entry->descriptorCount;
            }
            else if ((type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) ||
                     (type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER))
            {
                texel_buffer_view_count += entry->descriptorCount;
            }
            else
            {
                assert(false);
            }
        }

        // Compute start offsets for each type.
        size_t image_info_offset        = 0;
        size_t buffer_info_offset       = image_info_count * sizeof(VkDescriptorImageInfo);
        size_t texel_buffer_view_offset = buffer_info_offset + (buffer_info_count * sizeof(VkDescriptorBufferInfo));

        // Track descriptor image type.
        DescriptorImageTypes image_types;

        for (auto entry = entries.begin(); entry != entries.end(); ++entry)
        {
            VkDescriptorType type = entry->descriptorType;

            if ((type == VK_DESCRIPTOR_TYPE_SAMPLER) || (type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ||
                (type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) || (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) ||
                (type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT))
            {
                image_types.insert(image_types.end(), entry->descriptorCount, entry->descriptorType);

                entry->stride = sizeof(VkDescriptorImageInfo);
                entry->offset = image_info_offset;
                image_info_offset += entry->descriptorCount * sizeof(VkDescriptorImageInfo);
            }
            else if ((type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) || (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) ||
                     (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) ||
                     (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC))
            {
                entry->stride = sizeof(VkDescriptorBufferInfo);
                entry->offset = buffer_info_offset;
                buffer_info_offset += entry->descriptorCount * sizeof(VkDescriptorBufferInfo);
            }
            else if ((type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) ||
                     (type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER))
            {
                entry->stride = sizeof(VkBufferView);
                entry->offset = texel_buffer_view_offset;
                texel_buffer_view_offset += entry->descriptorCount * sizeof(VkBufferView);
            }
            else
            {
                assert(false);
            }
        }

        override_create_info.pDescriptorUpdateEntries = entries.data();

        VkResult result = func(device, &override_create_info, pAllocator, pDescriptorUpdateTemplate);

        if ((result == VK_SUCCESS) && (pDescriptorUpdateTemplate != nullptr) &&
            (*pDescriptorUpdateTemplate != VK_NULL_HANDLE))
        {
            descriptor_update_template_image_types_.emplace(*pDescriptorUpdateTemplate, image_types);
        }

        return result;
    }
    else
    {
        return func(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
    }
}

void VulkanReplayConsumerBase::OverrideDestroyDescriptorUpdateTemplate(
    PFN_vkDestroyDescriptorUpdateTemplate func,
    VkDevice                              device,
    VkDescriptorUpdateTemplate            descriptorUpdateTemplate,
    const VkAllocationCallbacks*          pAllocator)
{
    descriptor_update_template_image_types_.erase(descriptorUpdateTemplate);
    func(device, descriptorUpdateTemplate, pAllocator);
}

VkResult VulkanReplayConsumerBase::OverrideCreatePipelineCache(
    PFN_vkCreatePipelineCache                    func,
    VkResult                                     original_result,
    VkDevice                                     device,
    const VkPipelineCacheCreateInfo*             pCreateInfo,
    const VkAllocationCallbacks*                 pAllocator,
    const HandlePointerDecoder<VkPipelineCache>& original_pipeline_cache,
    VkPipelineCache*                             pPipelineCache)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(original_pipeline_cache);

    if (options_.omit_pipeline_cache_data && (pCreateInfo != nullptr))
    {
        // Make a shallow copy of the create info structure and clear the cache data.
        VkPipelineCacheCreateInfo override_create_info = *pCreateInfo;
        override_create_info.initialDataSize           = 0;
        override_create_info.pInitialData              = nullptr;

        return func(device, &override_create_info, pAllocator, pPipelineCache);
    }
    else
    {
        return func(device, pCreateInfo, pAllocator, pPipelineCache);
    }
}

VkResult VulkanReplayConsumerBase::OverrideCreateDebugReportCallbackEXT(
    PFN_vkCreateDebugReportCallbackEXT                    func,
    VkResult                                              original_result,
    VkInstance                                            instance,
    const VkDebugReportCallbackCreateInfoEXT*             pCreateInfo,
    const VkAllocationCallbacks*                          pAllocator,
    const HandlePointerDecoder<VkDebugReportCallbackEXT>& original_callback,
    VkDebugReportCallbackEXT*                             pCallback)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(original_callback);

    VkDebugReportCallbackCreateInfoEXT modified_create_info{};
    if (pCreateInfo != nullptr)
    {
        modified_create_info             = (*pCreateInfo);
        modified_create_info.pfnCallback = DebugReportCallback;
    }
    else
    {
        GFXRECON_LOG_WARNING("The vkCreateDebugReportCallbackEXT parameter pCreateInfo is NULL.");
    }

    return func(instance, &modified_create_info, pAllocator, pCallback);
}

VkResult VulkanReplayConsumerBase::OverrideCreateDebugUtilsMessengerEXT(
    PFN_vkCreateDebugUtilsMessengerEXT                    func,
    VkResult                                              original_result,
    VkInstance                                            instance,
    const VkDebugUtilsMessengerCreateInfoEXT*             pCreateInfo,
    const VkAllocationCallbacks*                          pAllocator,
    const HandlePointerDecoder<VkDebugUtilsMessengerEXT>& original_messenger,
    VkDebugUtilsMessengerEXT*                             pMessenger)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(original_messenger);

    VkDebugUtilsMessengerCreateInfoEXT modified_create_info{};
    if (pCreateInfo != nullptr)
    {
        modified_create_info                 = (*pCreateInfo);
        modified_create_info.pfnUserCallback = DebugUtilsCallback;
    }
    else
    {
        GFXRECON_LOG_WARNING("The vkCreateDebugUtilsMessengerEXT parameter pCreateInfo is NULL.");
    }

    return func(instance, &modified_create_info, pAllocator, pMessenger);
}

VkResult
VulkanReplayConsumerBase::OverrideCreateSwapchainKHR(PFN_vkCreateSwapchainKHR                    func,
                                                     VkResult                                    original_result,
                                                     VkDevice                                    device,
                                                     const VkSwapchainCreateInfoKHR*             pCreateInfo,
                                                     const VkAllocationCallbacks*                pAllocator,
                                                     const HandlePointerDecoder<VkSwapchainKHR>& original_swapchain,
                                                     VkSwapchainKHR*                             pSwapchain)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(original_swapchain);

    VkResult result = func(device, pCreateInfo, pAllocator, pSwapchain);

    if ((result == VK_SUCCESS) && (pCreateInfo) && (pSwapchain != nullptr))
    {
        if ((pCreateInfo->imageSharingMode == VK_SHARING_MODE_CONCURRENT) && (pCreateInfo->queueFamilyIndexCount > 0) &&
            (pCreateInfo->pQueueFamilyIndices != nullptr))
        {
            swapchain_queue_families_[*pSwapchain] = pCreateInfo->pQueueFamilyIndices[0];
        }
        else
        {
            swapchain_queue_families_[*pSwapchain] = 0;
        }

        swapchain_surfaces_[*pSwapchain] = pCreateInfo->surface;
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideAcquireNextImageKHR(PFN_vkAcquireNextImageKHR       func,
                                                               VkResult                        original_result,
                                                               VkDevice                        device,
                                                               VkSwapchainKHR                  swapchain,
                                                               uint64_t                        timeout,
                                                               VkSemaphore                     semaphore,
                                                               VkFence                         fence,
                                                               const PointerDecoder<uint32_t>& original_index,
                                                               uint32_t*                       pImageIndex)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    assert(original_index.GetPointer() != nullptr);

    VkResult    result               = VK_SUCCESS;
    uint32_t    captured_index       = *original_index.GetPointer();
    VkSemaphore preacquire_semaphore = VK_NULL_HANDLE;
    VkFence     preacquire_fence     = VK_NULL_HANDLE;

    if (swapchain_image_tracker_.RetrievePreAcquiredImage(
            swapchain, captured_index, &preacquire_semaphore, &preacquire_fence))
    {
        auto table = GetDeviceTable(device);
        assert(table != nullptr);

        // The image has already been acquired. Swap the synchronization objects.
        if (semaphore != VK_NULL_HANDLE)
        {
            // TODO: This should be processed at a higher level where the original handle IDs are available, so that the
            // swap can be performed with the original handle ID and the semaphore can be guaranteed not to be used
            // after destroy.
            object_mapper_.ReplaceSemaphore(semaphore, preacquire_semaphore);
            preacquire_semaphore = semaphore;
        }

        if (fence != VK_NULL_HANDLE)
        {
            // TODO: This should be processed at a higher level where the original handle IDs are available, so that the
            // swap can be performed with the original handle ID and the fence can be guaranteed not to be used
            // after destroy.
            object_mapper_.ReplaceFence(fence, preacquire_fence);
            preacquire_fence = fence;
        }

        table->DestroySemaphore(device, preacquire_semaphore, nullptr);
        table->DestroyFence(device, preacquire_fence, nullptr);
    }
    else
    {
        result = func(device, swapchain, timeout, semaphore, fence, pImageIndex);
    }

    return result;
}

VkResult VulkanReplayConsumerBase::OverrideAcquireNextImage2KHR(PFN_vkAcquireNextImage2KHR       func,
                                                                VkResult                         original_result,
                                                                VkDevice                         device,
                                                                const VkAcquireNextImageInfoKHR* pAcquireInfo,
                                                                const PointerDecoder<uint32_t>&  original_index,
                                                                uint32_t*                        pImageIndex)
{
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    assert(pAcquireInfo != nullptr);
    assert(original_index.GetPointer() != nullptr);

    VkResult    result               = VK_SUCCESS;
    uint32_t    captured_index       = *original_index.GetPointer();
    VkSemaphore preacquire_semaphore = VK_NULL_HANDLE;
    VkFence     preacquire_fence     = VK_NULL_HANDLE;

    if (swapchain_image_tracker_.RetrievePreAcquiredImage(
            pAcquireInfo->swapchain, captured_index, &preacquire_semaphore, &preacquire_fence))
    {
        auto table = GetDeviceTable(device);
        assert(table != nullptr);

        // The image has already been acquired. Swap the synchronization objects.
        if (pAcquireInfo->semaphore != VK_NULL_HANDLE)
        {
            // TODO: This should be processed at a higher level where the original handle IDs are available, so that the
            // swap can be performed with the original handle ID and the semaphore can be guaranteed not to be used
            // after destroy.
            object_mapper_.ReplaceSemaphore(pAcquireInfo->semaphore, preacquire_semaphore);
            preacquire_semaphore = pAcquireInfo->semaphore;
        }

        if (pAcquireInfo->fence != VK_NULL_HANDLE)
        {
            // TODO: This should be processed at a higher level where the original handle IDs are available, so that the
            // swap can be performed with the original handle ID and the fence can be guaranteed not to be used
            // after destroy.
            object_mapper_.ReplaceFence(pAcquireInfo->fence, preacquire_fence);
            preacquire_fence = pAcquireInfo->fence;
        }

        table->DestroySemaphore(device, preacquire_semaphore, nullptr);
        table->DestroyFence(device, preacquire_fence, nullptr);
    }
    else
    {
        result = func(device, pAcquireInfo, pImageIndex);
    }

    return result;
}

VkResult
VulkanReplayConsumerBase::OverrideCreateAndroidSurfaceKHR(PFN_vkCreateAndroidSurfaceKHR             func,
                                                          VkResult                                  original_result,
                                                          VkInstance                                instance,
                                                          const VkAndroidSurfaceCreateInfoKHR*      pCreateInfo,
                                                          const VkAllocationCallbacks*              pAllocator,
                                                          const HandlePointerDecoder<VkSurfaceKHR>& original_surface,
                                                          VkSurfaceKHR*                             pSurface)
{
    GFXRECON_UNREFERENCED_PARAMETER(func);
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(pAllocator);
    GFXRECON_UNREFERENCED_PARAMETER(original_surface);
    return CreateSurface(instance, pCreateInfo->flags, pSurface);
}

VkResult
VulkanReplayConsumerBase::OverrideCreateWin32SurfaceKHR(PFN_vkCreateWin32SurfaceKHR               func,
                                                        VkResult                                  original_result,
                                                        VkInstance                                instance,
                                                        const VkWin32SurfaceCreateInfoKHR*        pCreateInfo,
                                                        const VkAllocationCallbacks*              pAllocator,
                                                        const HandlePointerDecoder<VkSurfaceKHR>& original_surface,
                                                        VkSurfaceKHR*                             pSurface)
{
    GFXRECON_UNREFERENCED_PARAMETER(func);
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(pAllocator);
    GFXRECON_UNREFERENCED_PARAMETER(original_surface);
    return CreateSurface(instance, pCreateInfo->flags, pSurface);
}

VkBool32 VulkanReplayConsumerBase::OverrideGetPhysicalDeviceWin32PresentationSupportKHR(
    PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR func, VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex)
{
    GFXRECON_UNREFERENCED_PARAMETER(func);
    return window_factory_->GetPhysicalDevicePresentationSupport(
        GetInstanceTable(physicalDevice), physicalDevice, queueFamilyIndex);
}

VkResult
VulkanReplayConsumerBase::OverrideCreateXcbSurfaceKHR(PFN_vkCreateXcbSurfaceKHR                 func,
                                                      VkResult                                  original_result,
                                                      VkInstance                                instance,
                                                      const VkXcbSurfaceCreateInfoKHR*          pCreateInfo,
                                                      const VkAllocationCallbacks*              pAllocator,
                                                      const HandlePointerDecoder<VkSurfaceKHR>& original_surface,
                                                      VkSurfaceKHR*                             pSurface)
{
    GFXRECON_UNREFERENCED_PARAMETER(func);
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(pAllocator);
    GFXRECON_UNREFERENCED_PARAMETER(original_surface);
    return CreateSurface(instance, pCreateInfo->flags, pSurface);
}

VkBool32 VulkanReplayConsumerBase::OverrideGetPhysicalDeviceXcbPresentationSupportKHR(
    PFN_vkGetPhysicalDeviceXcbPresentationSupportKHR func,
    VkPhysicalDevice                                 physicalDevice,
    uint32_t                                         queueFamilyIndex,
    xcb_connection_t*                                connection,
    xcb_visualid_t                                   visual_id)
{
    GFXRECON_UNREFERENCED_PARAMETER(func);
    GFXRECON_UNREFERENCED_PARAMETER(connection);
    GFXRECON_UNREFERENCED_PARAMETER(visual_id);
    return window_factory_->GetPhysicalDevicePresentationSupport(
        GetInstanceTable(physicalDevice), physicalDevice, queueFamilyIndex);
}

VkResult
VulkanReplayConsumerBase::OverrideCreateXlibSurfaceKHR(PFN_vkCreateXlibSurfaceKHR                func,
                                                       VkResult                                  original_result,
                                                       VkInstance                                instance,
                                                       const VkXlibSurfaceCreateInfoKHR*         pCreateInfo,
                                                       const VkAllocationCallbacks*              pAllocator,
                                                       const HandlePointerDecoder<VkSurfaceKHR>& original_surface,
                                                       VkSurfaceKHR*                             pSurface)
{
    GFXRECON_UNREFERENCED_PARAMETER(func);
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(pAllocator);
    GFXRECON_UNREFERENCED_PARAMETER(original_surface);
    return CreateSurface(instance, pCreateInfo->flags, pSurface);
}

VkBool32 VulkanReplayConsumerBase::OverrideGetPhysicalDeviceXlibPresentationSupportKHR(
    PFN_vkGetPhysicalDeviceXlibPresentationSupportKHR func,
    VkPhysicalDevice                                  physicalDevice,
    uint32_t                                          queueFamilyIndex,
    Display*                                          dpy,
    VisualID                                          visualID)
{
    GFXRECON_UNREFERENCED_PARAMETER(func);
    GFXRECON_UNREFERENCED_PARAMETER(dpy);
    GFXRECON_UNREFERENCED_PARAMETER(visualID);
    return window_factory_->GetPhysicalDevicePresentationSupport(
        GetInstanceTable(physicalDevice), physicalDevice, queueFamilyIndex);
}

VkResult
VulkanReplayConsumerBase::OverrideCreateWaylandSurfaceKHR(PFN_vkCreateWaylandSurfaceKHR             func,
                                                          VkResult                                  original_result,
                                                          VkInstance                                instance,
                                                          const VkWaylandSurfaceCreateInfoKHR*      pCreateInfo,
                                                          const VkAllocationCallbacks*              pAllocator,
                                                          const HandlePointerDecoder<VkSurfaceKHR>& original_surface,
                                                          VkSurfaceKHR*                             pSurface)
{
    GFXRECON_UNREFERENCED_PARAMETER(func);
    GFXRECON_UNREFERENCED_PARAMETER(original_result);
    GFXRECON_UNREFERENCED_PARAMETER(pAllocator);
    GFXRECON_UNREFERENCED_PARAMETER(original_surface);
    return CreateSurface(instance, pCreateInfo->flags, pSurface);
}

VkBool32 VulkanReplayConsumerBase::OverrideGetPhysicalDeviceWaylandPresentationSupportKHR(
    PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR func,
    VkPhysicalDevice                                     physicalDevice,
    uint32_t                                             queueFamilyIndex,
    struct wl_display*                                   display)
{
    GFXRECON_UNREFERENCED_PARAMETER(func);
    GFXRECON_UNREFERENCED_PARAMETER(display);
    return window_factory_->GetPhysicalDevicePresentationSupport(
        GetInstanceTable(physicalDevice), physicalDevice, queueFamilyIndex);
}

void VulkanReplayConsumerBase::OverrideDestroySurfaceKHR(PFN_vkDestroySurfaceKHR      func,
                                                         VkInstance                   instance,
                                                         VkSurfaceKHR                 surface,
                                                         const VkAllocationCallbacks* pAllocator)
{
    func(instance, surface, pAllocator);

    if (surface != VK_NULL_HANDLE)
    {
        auto entry = window_map_.find(surface);

        if (entry != window_map_.end())
        {
            window_factory_->Destroy(entry->second);
            window_map_.erase(entry);
        }
    }
}

void VulkanReplayConsumerBase::MapDescriptorUpdateTemplateHandles(VkDescriptorUpdateTemplate update_template,
                                                                  const DescriptorUpdateTemplateDecoder& decoder)
{
    size_t image_info_count        = decoder.GetImageInfoCount();
    size_t buffer_info_count       = decoder.GetBufferInfoCount();
    size_t texel_buffer_view_count = decoder.GetTexelBufferViewCount();

    if (image_info_count > 0)
    {
        auto image_types_iter = descriptor_update_template_image_types_.find(update_template);
        if (image_types_iter != descriptor_update_template_image_types_.end())
        {
            Decoded_VkDescriptorImageInfo* structs = decoder.GetImageInfoMetaStructPointer();
            for (size_t i = 0; i < image_info_count; ++i)
            {
                MapStructHandles(image_types_iter->second[i], &structs[i], object_mapper_);
            }
        }
        else
        {
            GFXRECON_LOG_ERROR("Missing descriptor update template image descriptor type info; attempting to map both "
                               "VkDescriptorImageInfo handles");

            Decoded_VkDescriptorImageInfo* structs = decoder.GetImageInfoMetaStructPointer();
            for (size_t i = 0; i < image_info_count; ++i)
            {
                MapStructHandles(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &structs[i], object_mapper_);
            }
        }
    }

    if (buffer_info_count > 0)
    {
        MapStructArrayHandles(decoder.GetBufferInfoMetaStructPointer(), buffer_info_count, object_mapper_);
    }

    if (texel_buffer_view_count > 0)
    {
        MapHandles<VkBufferView>(decoder.GetTexelBufferViewHandleIdsPointer(),
                                 texel_buffer_view_count,
                                 decoder.GetTexelBufferViewPointer(),
                                 texel_buffer_view_count,
                                 &VulkanObjectMapper::MapVkBufferView);
    }
}

void VulkanReplayConsumerBase::Process_vkUpdateDescriptorSetWithTemplate(format::HandleId device,
                                                                         format::HandleId descriptorSet,
                                                                         format::HandleId descriptorUpdateTemplate,
                                                                         const DescriptorUpdateTemplateDecoder& pData)
{
    VkDevice                   in_device        = object_mapper_.MapVkDevice(device);
    VkDescriptorSet            in_descriptorSet = object_mapper_.MapVkDescriptorSet(descriptorSet);
    VkDescriptorUpdateTemplate in_descriptorUpdateTemplate =
        object_mapper_.MapVkDescriptorUpdateTemplate(descriptorUpdateTemplate);

    MapDescriptorUpdateTemplateHandles(in_descriptorUpdateTemplate, pData);

    GetDeviceTable(in_device)->UpdateDescriptorSetWithTemplate(
        in_device, in_descriptorSet, in_descriptorUpdateTemplate, pData.GetPointer());
}

void VulkanReplayConsumerBase::Process_vkCmdPushDescriptorSetWithTemplateKHR(
    format::HandleId                       commandBuffer,
    format::HandleId                       descriptorUpdateTemplate,
    format::HandleId                       layout,
    uint32_t                               set,
    const DescriptorUpdateTemplateDecoder& pData)
{
    VkCommandBuffer            in_commandBuffer = object_mapper_.MapVkCommandBuffer(commandBuffer);
    VkDescriptorUpdateTemplate in_descriptorUpdateTemplate =
        object_mapper_.MapVkDescriptorUpdateTemplate(descriptorUpdateTemplate);
    VkPipelineLayout in_layout = object_mapper_.MapVkPipelineLayout(layout);

    MapDescriptorUpdateTemplateHandles(in_descriptorUpdateTemplate, pData);

    GetDeviceTable(in_commandBuffer)
        ->CmdPushDescriptorSetWithTemplateKHR(
            in_commandBuffer, in_descriptorUpdateTemplate, in_layout, set, pData.GetPointer());
}

void VulkanReplayConsumerBase::Process_vkUpdateDescriptorSetWithTemplateKHR(
    format::HandleId                       device,
    format::HandleId                       descriptorSet,
    format::HandleId                       descriptorUpdateTemplate,
    const DescriptorUpdateTemplateDecoder& pData)
{
    VkDevice                   in_device        = object_mapper_.MapVkDevice(device);
    VkDescriptorSet            in_descriptorSet = object_mapper_.MapVkDescriptorSet(descriptorSet);
    VkDescriptorUpdateTemplate in_descriptorUpdateTemplate =
        object_mapper_.MapVkDescriptorUpdateTemplate(descriptorUpdateTemplate);

    MapDescriptorUpdateTemplateHandles(in_descriptorUpdateTemplate, pData);

    GetDeviceTable(in_device)->UpdateDescriptorSetWithTemplateKHR(
        in_device, in_descriptorSet, in_descriptorUpdateTemplate, pData.GetPointer());
}

void VulkanReplayConsumerBase::Process_vkRegisterObjectsNVX(
    VkResult                                                   returnValue,
    format::HandleId                                           device,
    format::HandleId                                           objectTable,
    uint32_t                                                   objectCount,
    const StructPointerDecoder<Decoded_VkObjectTableEntryNVX>& ppObjectTableEntries,
    const PointerDecoder<uint32_t>&                            pObjectIndices)
{
    VkDevice         in_device         = object_mapper_.MapVkDevice(device);
    VkObjectTableNVX in_objectTable    = object_mapper_.MapVkObjectTableNVX(objectTable);
    const uint32_t*  in_pObjectIndices = reinterpret_cast<const uint32_t*>(pObjectIndices.GetPointer());

    assert(objectCount == ppObjectTableEntries.GetLength());

    VkObjectTableEntryNVX**         in_ppObjectTableEntries         = ppObjectTableEntries.GetPointer();
    Decoded_VkObjectTableEntryNVX** in_ppObjectTableEntries_wrapper = ppObjectTableEntries.GetMetaStructPointer();

    // Map the object table entry handles.
    if ((in_ppObjectTableEntries != nullptr) && (in_ppObjectTableEntries_wrapper != nullptr))
    {
        for (size_t i = 0; i < objectCount; ++i)
        {
            if (in_ppObjectTableEntries[i] != nullptr)
            {
                VkObjectEntryTypeNVX type = in_ppObjectTableEntries[i]->type;

                if (type == VK_OBJECT_ENTRY_TYPE_DESCRIPTOR_SET_NVX)
                {
                    MapStructHandles(reinterpret_cast<Decoded_VkObjectTableDescriptorSetEntryNVX*>(
                                         in_ppObjectTableEntries_wrapper[i]),
                                     object_mapper_);
                }
                else if (type == VK_OBJECT_ENTRY_TYPE_PIPELINE_NVX)
                {
                    MapStructHandles(
                        reinterpret_cast<Decoded_VkObjectTablePipelineEntryNVX*>(in_ppObjectTableEntries_wrapper[i]),
                        object_mapper_);
                }
                else if (type == VK_OBJECT_ENTRY_TYPE_INDEX_BUFFER_NVX)
                {
                    MapStructHandles(
                        reinterpret_cast<Decoded_VkObjectTableIndexBufferEntryNVX*>(in_ppObjectTableEntries_wrapper[i]),
                        object_mapper_);
                }
                else if (type == VK_OBJECT_ENTRY_TYPE_VERTEX_BUFFER_NVX)
                {
                    MapStructHandles(reinterpret_cast<Decoded_VkObjectTableVertexBufferEntryNVX*>(
                                         in_ppObjectTableEntries_wrapper[i]),
                                     object_mapper_);
                }
                else if (type == VK_OBJECT_ENTRY_TYPE_PUSH_CONSTANT_NVX)
                {
                    MapStructHandles(reinterpret_cast<Decoded_VkObjectTablePushConstantEntryNVX*>(
                                         in_ppObjectTableEntries_wrapper[i]),
                                     object_mapper_);
                }
                else
                {
                    assert(true);
                    GFXRECON_LOG_ERROR("Skipping entry with unrecognized type at ppObjectTableEntries[%d] when "
                                       "processing vkRegisterObjectsNVX",
                                       i);
                }
            }
            else
            {
                GFXRECON_LOG_WARNING(
                    "Skipping null entry at ppObjectTableEntries[%d] when processing vkRegisterObjectsNVX", i);
            }
        }
    }

    VkResult replay_result = GetDeviceTable(in_device)->RegisterObjectsNVX(
        in_device, in_objectTable, objectCount, in_ppObjectTableEntries, in_pObjectIndices);

    CheckResult("vkRegisterObjectsNVX", returnValue, replay_result);
}

GFXRECON_END_NAMESPACE(decode)
GFXRECON_END_NAMESPACE(gfxrecon)
