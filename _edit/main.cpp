// Copyright 2020 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <fileformats/stb_image_write.h>
#include <nvh/fileoperations.hpp>  // For nvh::loadFile

#define NVVK_ALLOC_DEDICATED
#include <nvvk/allocator_vk.hpp>  // For NVVK memory allocators
#include <nvvk/context_vk.hpp>
#include <nvvk/descriptorsets_vk.hpp> // For nvvk::DescriptorSetContainer
#include <nvvk/shaders_vk.hpp>  // For nvvk::createShaderModule
#include <nvvk/structs_vk.hpp> // Using nvvk::make

static const uint64_t render_width = 800;
static const uint64_t render_height = 600;
static const uint32_t workgroup_width = 16;
static const uint32_t workgroup_height = 8;

int main(int argc, const char** argv)
{
  nvvk::ContextCreateInfo deviceInfo;
  deviceInfo.apiMajor = 1;
  deviceInfo.apiMinor = 2;

  // Required by BK_KHR_ray_query; allows work to be offloaded into background threads and parallelized
  deviceInfo.addDeviceExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
  VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures = nvvk::make<VkPhysicalDeviceAccelerationStructureFeaturesKHR>();
  deviceInfo.addDeviceExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, false, &asFeatures);
  VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures = nvvk::make<VkPhysicalDeviceRayQueryFeaturesKHR>();
  deviceInfo.addDeviceExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME, false, &rayQueryFeatures);

  nvvk::Context context; // Encasulates the device state
  context.init(deviceInfo);
  // Device must support acceleration structures and ray queries:
  assert(asFeatures.accelerationStructure == VK_TRUE && rayQueryFeatures.rayQuery == VK_TRUE);

  // Create the allocator; note that nvvk::Context has an implicit cast to VkDevice
  nvvk::AllocatorDedicated allocator;
  allocator.init(context, context.m_physicalDevice);

  // Create a buffer
  VkDeviceSize bufferSizeBytes = render_width * render_height * 3 * sizeof(float);
  VkBufferCreateInfo bufferCreateInfo = nvvk::make<VkBufferCreateInfo>();
  bufferCreateInfo.size = bufferSizeBytes;
  bufferCreateInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  // VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT means that the CPU can read this buffer's memory.
  // VK_MEMORY_PROPERTY_HOST_CACHED_BIT means that the CPU caches this memory.
  // VK_MEMORY_PROPERTY_HOST_COHERENT_BIT means that the CPU side of cache management
  // is handled automatically, with potentially slower reads/writes.

  nvvk::BufferDedicated buffer = allocator.createBuffer(bufferCreateInfo, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  const std::string exePath(argv[0], std::string(argv[0]).find_last_of("/\\") + 1);
  std::vector<std::string> searchPaths = { exePath + PROJECT_RELDIRECTORY, exePath + PROJECT_RELDIRECTORY "..",
                                          exePath + PROJECT_RELDIRECTORY "../..", exePath + PROJECT_NAME };

  // Create the command pool
  VkCommandPoolCreateInfo cmdPoolCreateInfo = nvvk::make<VkCommandPoolCreateInfo>();
  cmdPoolCreateInfo.queueFamilyIndex = context.m_queueGCT;
  VkCommandPool cmdPool;
  NVVK_CHECK(vkCreateCommandPool(context, &cmdPoolCreateInfo, nullptr, &cmdPool));

  // Here's the list of bindings for the descriptor set layout, from raytrace.comp.glsl:
  // 0 - a storage buffer (the buffer `buffer`)
  nvvk::DescriptorSetContainer descriptorSetContainer(context);
  descriptorSetContainer.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);

  // Create a layout from the list of bindings
  descriptorSetContainer.initLayout();

  // Create a descriptor pool from the list of bindings with space for one set, and all the set
  descriptorSetContainer.initPool(1);
  // Create a simple pipeline layout from the descriptor set layout
  descriptorSetContainer.initPipeLayout();

  // Write a single descriptor in the descriptor set
  VkDescriptorBufferInfo descriptorBufferInfo{};
  descriptorBufferInfo.buffer = buffer.buffer;
  descriptorBufferInfo.range = bufferSizeBytes;
  VkWriteDescriptorSet writeDescriptor = descriptorSetContainer.makeWrite(0 /*set index*/, 0 /*binding*/, &descriptorBufferInfo);
  vkUpdateDescriptorSets(context,              // The context
      1, &writeDescriptor,  // An array of VkWriteDescriptorSet objects
      0, nullptr);          // An array of VkCopyDescriptorSet objects (unused)

  // Shader loading and pipeline creation
  VkShaderModule rayTraceModule = nvvk::createShaderModule(context, nvh::loadFile("shaders/raytrace.comp.glsl.spv", true, searchPaths));

  // Describe the entrypoint and the stage to use this shader module in the pipeline
  VkPipelineShaderStageCreateInfo shaderStageCreateInfo = nvvk::make<VkPipelineShaderStageCreateInfo>();
  shaderStageCreateInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  shaderStageCreateInfo.module = rayTraceModule;
  shaderStageCreateInfo.pName = "main";

  // Create compute pipeline
  VkComputePipelineCreateInfo pipelineCreateInfo = nvvk::make<VkComputePipelineCreateInfo>();
  pipelineCreateInfo.stage = shaderStageCreateInfo;
  pipelineCreateInfo.layout = descriptorSetContainer.getPipeLayout();
  // Don't modify flags, basePipelineHandle, or basePipelineIndex
  VkPipeline computePipeline;
  NVVK_CHECK(vkCreateComputePipelines(context,                 // Device
      VK_NULL_HANDLE,          // Pipeline cache (uses default)
      1, &pipelineCreateInfo,  // Compute pipeline create info
      VK_NULL_HANDLE,          // Allocator (uses default)
      &computePipeline));      // Output

  // Allocate a command buffer
  VkCommandBufferAllocateInfo cmdAllocInfo = nvvk::make<VkCommandBufferAllocateInfo>();
  cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdAllocInfo.commandPool = cmdPool;
  cmdAllocInfo.commandBufferCount = 1;
  VkCommandBuffer cmdBuffer;
  NVVK_CHECK(vkAllocateCommandBuffers(context, &cmdAllocInfo, &cmdBuffer));

  // Begin recording
  VkCommandBufferBeginInfo beginInfo = nvvk::make<VkCommandBufferBeginInfo>();
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  NVVK_CHECK(vkBeginCommandBuffer(cmdBuffer, &beginInfo));

  // Bind the compute pipeline
  vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);

  // Bind the descriptor set
  VkDescriptorSet descriptorSet = descriptorSetContainer.getSet(0);
  vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, descriptorSetContainer.getPipeLayout(), 0, 1, &descriptorSet, 0, nullptr);

  // Run the compute shader with enough workgroups to cover the entire buffer:
  vkCmdDispatch(cmdBuffer, (uint32_t(render_width) + workgroup_width - 1) / workgroup_width, (uint32_t(render_height) + workgroup_height - 1) / workgroup_height, 1);

  // Ensure that memory writes by the vkCmdFillBuffer call
  // are available to read from the CPU." (In other words, "Flush the GPU caches
  // so the CPU can read the data.")
  VkMemoryBarrier memoryBarrier = nvvk::make<VkMemoryBarrier>();
  memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;  // Make shader writes
  memoryBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;     // Readable by the CPU
  vkCmdPipelineBarrier(cmdBuffer,                             // The command buffer
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,   // From the compute shader
      VK_PIPELINE_STAGE_HOST_BIT,             // To the CPU
      0,                                      // No special flags
      1, &memoryBarrier,                      // An array of memory barriers
      0, nullptr, 0, nullptr);                // No other barriers

  // End recording
  NVVK_CHECK(vkEndCommandBuffer(cmdBuffer));

  // Submit the command buffer
  VkSubmitInfo submitInfo = nvvk::make<VkSubmitInfo>();
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmdBuffer;
  NVVK_CHECK(vkQueueSubmit(context.m_queueGCT, 1, &submitInfo, VK_NULL_HANDLE));

  // Wait for the GPU to finish
  NVVK_CHECK(vkQueueWaitIdle(context.m_queueGCT));

  // Get the image data back from the GPU
  void* data = allocator.map(buffer);
  stbi_write_hdr("out.hdr", render_width, render_height, 3, reinterpret_cast<float*>(data));
  allocator.unmap(buffer);

  vkDestroyPipeline(context ,computePipeline, nullptr);
  vkDestroyShaderModule(context, rayTraceModule, nullptr);
  descriptorSetContainer.deinit();
  vkFreeCommandBuffers(context, cmdPool, 1, &cmdBuffer);
  vkDestroyCommandPool(context, cmdPool, nullptr);
  allocator.destroy(buffer);
  allocator.deinit();
  context.deinit();
}