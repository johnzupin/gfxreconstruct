/*
** Copyright (c) 2018 Valve Corporation
** Copyright (c) 2018 LunarG, Inc.
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

#include "decode/vulkan_decoder_base.h"

#include "decode/descriptor_update_template_decoder.h"
#include "decode/pointer_decoder.h"
#include "decode/value_decoder.h"

GFXRECON_BEGIN_NAMESPACE(gfxrecon)
GFXRECON_BEGIN_NAMESPACE(decode)

void VulkanDecoderBase::DispatchStateBeginMarker(uint64_t frame_number)
{
    for (auto consumer : consumers_)
    {
        consumer->ProcessStateBeginMarker(frame_number);
    }
}

void VulkanDecoderBase::DispatchStateEndMarker(uint64_t frame_number)
{
    for (auto consumer : consumers_)
    {
        consumer->ProcessStateEndMarker(frame_number);
    }
}

void VulkanDecoderBase::DispatchDisplayMessageCommand(format::ThreadId thread_id, const std::string& message)
{
    GFXRECON_UNREFERENCED_PARAMETER(thread_id);

    for (auto consumer : consumers_)
    {
        consumer->ProcessDisplayMessageCommand(message);
    }
}

void VulkanDecoderBase::DispatchFillMemoryCommand(
    format::ThreadId thread_id, uint64_t memory_id, uint64_t offset, uint64_t size, const uint8_t* data)
{
    GFXRECON_UNREFERENCED_PARAMETER(thread_id);

    for (auto consumer : consumers_)
    {
        consumer->ProcessFillMemoryCommand(memory_id, offset, size, data);
    }
}

void VulkanDecoderBase::DispatchResizeWindowCommand(format::ThreadId thread_id,
                                                    format::HandleId surface_id,
                                                    uint32_t         width,
                                                    uint32_t         height)
{
    GFXRECON_UNREFERENCED_PARAMETER(thread_id);

    for (auto consumer : consumers_)
    {
        consumer->ProcessResizeWindowCommand(surface_id, width, height);
    }
}

void VulkanDecoderBase::DispatchSetSwapchainImageStateCommand(
    format::ThreadId                                    thread_id,
    format::HandleId                                    device_id,
    format::HandleId                                    swapchain_id,
    uint32_t                                            last_presented_image,
    const std::vector<format::SwapchainImageStateInfo>& image_state)
{
    GFXRECON_UNREFERENCED_PARAMETER(thread_id);

    for (auto consumer : consumers_)
    {
        consumer->ProcessSetSwapchainImageStateCommand(device_id, swapchain_id, last_presented_image, image_state);
    }
}

void VulkanDecoderBase::DispatchBeginResourceInitCommand(format::ThreadId thread_id,
                                                         format::HandleId device_id,
                                                         uint64_t         max_resource_size,
                                                         uint64_t         max_copy_size)
{
    GFXRECON_UNREFERENCED_PARAMETER(thread_id);

    for (auto consumer : consumers_)
    {
        consumer->ProcessBeginResourceInitCommand(device_id, max_resource_size, max_copy_size);
    }
}

void VulkanDecoderBase::DispatchEndResourceInitCommand(format::ThreadId thread_id, format::HandleId device_id)
{
    GFXRECON_UNREFERENCED_PARAMETER(thread_id);

    for (auto consumer : consumers_)
    {
        consumer->ProcessEndResourceInitCommand(device_id);
    }
}

void VulkanDecoderBase::DispatchInitBufferCommand(format::ThreadId thread_id,
                                                  format::HandleId device_id,
                                                  format::HandleId buffer_id,
                                                  uint64_t         data_size,
                                                  const uint8_t*   data)
{
    GFXRECON_UNREFERENCED_PARAMETER(thread_id);

    for (auto consumer : consumers_)
    {
        consumer->ProcessInitBufferCommand(device_id, buffer_id, data_size, data);
    }
}

void VulkanDecoderBase::DispatchInitImageCommand(format::ThreadId             thread_id,
                                                 format::HandleId             device_id,
                                                 format::HandleId             image_id,
                                                 uint64_t                     data_size,
                                                 uint32_t                     aspect,
                                                 uint32_t                     layout,
                                                 const std::vector<uint64_t>& level_sizes,
                                                 const uint8_t*               data)
{
    GFXRECON_UNREFERENCED_PARAMETER(thread_id);

    for (auto consumer : consumers_)
    {
        consumer->ProcessInitImageCommand(device_id, image_id, data_size, aspect, layout, level_sizes, data);
    }
}

size_t VulkanDecoderBase::Decode_vkUpdateDescriptorSetWithTemplate(const uint8_t* parameter_buffer, size_t buffer_size)
{
    size_t bytes_read = 0;

    format::HandleId                device;
    format::HandleId                descriptorSet;
    format::HandleId                descriptorUpdateTemplate;
    DescriptorUpdateTemplateDecoder pData;

    bytes_read +=
        ValueDecoder::DecodeHandleIdValue((parameter_buffer + bytes_read), (buffer_size - bytes_read), &device);
    bytes_read +=
        ValueDecoder::DecodeHandleIdValue((parameter_buffer + bytes_read), (buffer_size - bytes_read), &descriptorSet);
    bytes_read += ValueDecoder::DecodeHandleIdValue(
        (parameter_buffer + bytes_read), (buffer_size - bytes_read), &descriptorUpdateTemplate);
    bytes_read += pData.Decode((parameter_buffer + bytes_read), (buffer_size - bytes_read));

    for (auto consumer : consumers_)
    {
        consumer->Process_vkUpdateDescriptorSetWithTemplate(device, descriptorSet, descriptorUpdateTemplate, pData);
    }

    return bytes_read;
}

size_t VulkanDecoderBase::Decode_vkCmdPushDescriptorSetWithTemplateKHR(const uint8_t* parameter_buffer,
                                                                       size_t         buffer_size)
{
    size_t bytes_read = 0;

    format::HandleId                commandBuffer;
    format::HandleId                descriptorUpdateTemplate;
    format::HandleId                layout;
    uint32_t                        set;
    DescriptorUpdateTemplateDecoder pData;

    bytes_read +=
        ValueDecoder::DecodeHandleIdValue((parameter_buffer + bytes_read), (buffer_size - bytes_read), &commandBuffer);
    bytes_read += ValueDecoder::DecodeHandleIdValue(
        (parameter_buffer + bytes_read), (buffer_size - bytes_read), &descriptorUpdateTemplate);
    bytes_read +=
        ValueDecoder::DecodeHandleIdValue((parameter_buffer + bytes_read), (buffer_size - bytes_read), &layout);
    bytes_read += ValueDecoder::DecodeUInt32Value((parameter_buffer + bytes_read), (buffer_size - bytes_read), &set);
    bytes_read += pData.Decode((parameter_buffer + bytes_read), (buffer_size - bytes_read));

    for (auto consumer : consumers_)
    {
        consumer->Process_vkCmdPushDescriptorSetWithTemplateKHR(
            commandBuffer, descriptorUpdateTemplate, layout, set, pData);
    }

    return bytes_read;
}

size_t VulkanDecoderBase::Decode_vkUpdateDescriptorSetWithTemplateKHR(const uint8_t* parameter_buffer,
                                                                      size_t         buffer_size)
{
    size_t bytes_read = 0;

    format::HandleId                device;
    format::HandleId                descriptorSet;
    format::HandleId                descriptorUpdateTemplate;
    DescriptorUpdateTemplateDecoder pData;

    bytes_read +=
        ValueDecoder::DecodeHandleIdValue((parameter_buffer + bytes_read), (buffer_size - bytes_read), &device);
    bytes_read +=
        ValueDecoder::DecodeHandleIdValue((parameter_buffer + bytes_read), (buffer_size - bytes_read), &descriptorSet);
    bytes_read += ValueDecoder::DecodeHandleIdValue(
        (parameter_buffer + bytes_read), (buffer_size - bytes_read), &descriptorUpdateTemplate);
    bytes_read += pData.Decode((parameter_buffer + bytes_read), (buffer_size - bytes_read));

    for (auto consumer : consumers_)
    {
        consumer->Process_vkUpdateDescriptorSetWithTemplateKHR(device, descriptorSet, descriptorUpdateTemplate, pData);
    }

    return bytes_read;
}

size_t VulkanDecoderBase::Decode_vkRegisterObjectsNVX(const uint8_t* parameter_buffer, size_t buffer_size)
{
    size_t bytes_read = 0;

    format::HandleId                                    device;
    format::HandleId                                    objectTable;
    uint32_t                                            objectCount;
    StructPointerDecoder<Decoded_VkObjectTableEntryNVX> ppObjectTableEntries;
    PointerDecoder<uint32_t>                            pObjectIndices;
    VkResult                                            return_value;

    bytes_read +=
        ValueDecoder::DecodeHandleIdValue((parameter_buffer + bytes_read), (buffer_size - bytes_read), &device);
    bytes_read +=
        ValueDecoder::DecodeHandleIdValue((parameter_buffer + bytes_read), (buffer_size - bytes_read), &objectTable);
    bytes_read +=
        ValueDecoder::DecodeUInt32Value((parameter_buffer + bytes_read), (buffer_size - bytes_read), &objectCount);
    bytes_read += ppObjectTableEntries.Decode((parameter_buffer + bytes_read), (buffer_size - bytes_read));
    bytes_read += pObjectIndices.DecodeUInt32((parameter_buffer + bytes_read), (buffer_size - bytes_read));
    bytes_read +=
        ValueDecoder::DecodeEnumValue((parameter_buffer + bytes_read), (buffer_size - bytes_read), &return_value);

    for (auto consumer : consumers_)
    {
        consumer->Process_vkRegisterObjectsNVX(
            return_value, device, objectTable, objectCount, ppObjectTableEntries, pObjectIndices);
    }

    return bytes_read;
}

void VulkanDecoderBase::DecodeFunctionCall(format::ApiCallId  call_id,
                                           const ApiCallInfo& call_info,
                                           const uint8_t*     parameter_buffer,
                                           size_t             buffer_size)
{
    GFXRECON_UNREFERENCED_PARAMETER(call_info);

    switch (call_id)
    {
        case format::ApiCallId::ApiCall_vkUpdateDescriptorSetWithTemplate:
            Decode_vkUpdateDescriptorSetWithTemplate(parameter_buffer, buffer_size);
            break;
        case format::ApiCallId::ApiCall_vkCmdPushDescriptorSetWithTemplateKHR:
            Decode_vkCmdPushDescriptorSetWithTemplateKHR(parameter_buffer, buffer_size);
            break;
        case format::ApiCallId::ApiCall_vkUpdateDescriptorSetWithTemplateKHR:
            Decode_vkUpdateDescriptorSetWithTemplateKHR(parameter_buffer, buffer_size);
            break;
        case format::ApiCallId::ApiCall_vkRegisterObjectsNVX:
            Decode_vkRegisterObjectsNVX(parameter_buffer, buffer_size);
            break;
        default:
            break;
    }
}

GFXRECON_END_NAMESPACE(decode)
GFXRECON_END_NAMESPACE(gfxrecon)
