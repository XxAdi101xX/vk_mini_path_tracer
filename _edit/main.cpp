// Copyright 2020 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0
#include <array>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <fileformats/stb_image_write.h>
#define TINYOBJLOADER_IMPLEMENTATION
#include <fileformats/tiny_obj_loader.h>
#include <nvh/fileoperations.hpp>  // For nvh::loadFile
#define NVVK_ALLOC_DEDICATED
#include <nvvk/allocator_vk.hpp>  // For NVVK memory allocators
#include <nvvk/context_vk.hpp>
#include <nvvk/descriptorsets_vk.hpp>  // For nvvk::DescriptorSetContainer
#include <nvvk/raytraceKHR_vk.hpp>     // For nvvk::RaytracingBuilderKHR
#include <nvvk/shaders_vk.hpp>         // For nvvk::createShaderModule
#include <nvvk/structs_vk.hpp>         // For nvvk::make

#include "common.h"

VkCommandBuffer AllocateAndBeginOneTimeCommandBuffer(VkDevice device, VkCommandPool cmdPool)
{
  VkCommandBufferAllocateInfo cmdAllocInfo = nvvk::make<VkCommandBufferAllocateInfo>();
  cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdAllocInfo.commandPool = cmdPool;
  cmdAllocInfo.commandBufferCount = 1;
  VkCommandBuffer cmdBuffer;
  NVVK_CHECK(vkAllocateCommandBuffers(device, &cmdAllocInfo, &cmdBuffer));

  VkCommandBufferBeginInfo beginInfo = nvvk::make<VkCommandBufferBeginInfo>();
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  NVVK_CHECK(vkBeginCommandBuffer(cmdBuffer, &beginInfo));

  return cmdBuffer;
} 

void EndSubmitWaitAndFreeCommandBuffer(VkDevice device, VkQueue queue, VkCommandPool cmdPool, VkCommandBuffer& cmdBuffer)
{
    NVVK_CHECK(vkEndCommandBuffer(cmdBuffer));

    VkSubmitInfo submitInfo = nvvk::make<VkSubmitInfo>();
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;
    NVVK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
    NVVK_CHECK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuffer);
}

VkDeviceAddress GetBufferDeviceAddress(VkDevice device, VkBuffer buffer)
{
    VkBufferDeviceAddressInfo addressInfo = nvvk::make<VkBufferDeviceAddressInfo>();
    addressInfo.buffer = buffer;
    return vkGetBufferDeviceAddress(device, &addressInfo);
}

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
  VkDeviceSize bufferSizeBytes = RENDER_WIDTH * RENDER_HEIGHT * 3 * sizeof(float);
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
  tinyobj::ObjReader reader;  // Used to read an OBJ file
  reader.ParseFromFile(nvh::findFile("scenes/CornellBox-Original-Merged.obj", searchPaths));
  assert(reader.Valid());  // Make sure tinyobj was able to parse this file
  const std::vector<tinyobj::real_t> objVertices = reader.GetAttrib().GetVertices();
  const std::vector<tinyobj::shape_t>& objShapes = reader.GetShapes();  // All shapes in the file
  assert(objShapes.size() == 1);                                          // Check that this file has only one shape
  const tinyobj::shape_t& objShape = objShapes[0];                        // Get the first shape
  // Get the indices of the vertices of the first mesh of `objShape` in `attrib.vertices`:
  std::vector<uint32_t> objIndices;
  objIndices.reserve(objShape.mesh.indices.size());
  for (const tinyobj::index_t& index : objShape.mesh.indices)
  {
    objIndices.push_back(index.vertex_index);
  }

  // Create the command pool
  VkCommandPoolCreateInfo cmdPoolCreateInfo = nvvk::make<VkCommandPoolCreateInfo>();
  cmdPoolCreateInfo.queueFamilyIndex = context.m_queueGCT;
  VkCommandPool cmdPool;
  NVVK_CHECK(vkCreateCommandPool(context, &cmdPoolCreateInfo, nullptr, &cmdPool));

  // Upload the vertex and index buffers to the GPU
  nvvk::BufferDedicated vertexBuffer, indexBuffer;
  {
    // Start a command buffer for uplaoding the buffers
    VkCommandBuffer uploadCmdBuffer = AllocateAndBeginOneTimeCommandBuffer(context, cmdPool);
    // We get these buffers' device addresses, and use them as storage buffers and build inputs.
    const VkBufferUsageFlags usage = 
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

    // Note that the allocator is using a staging buffer internally to facilitate the transfer of data from the CPU to the GPU
    vertexBuffer = allocator.createBuffer(uploadCmdBuffer, objVertices, usage);
    indexBuffer = allocator.createBuffer(uploadCmdBuffer, objIndices, usage);

    EndSubmitWaitAndFreeCommandBuffer(context, context.m_queueGCT, cmdPool, uploadCmdBuffer);
    allocator.finalizeAndReleaseStaging();
  }

  // Describe the bottom-level acceleration structure (BLAS)
  std::vector<nvvk::RaytracingBuilderKHR::BlasInput> blases;
  {
    nvvk::RaytracingBuilderKHR::BlasInput blas;
    // Get the device addresses of the vertex and index buffers
    VkDeviceAddress vertexBufferAddress = GetBufferDeviceAddress(context, vertexBuffer.buffer);
    VkDeviceAddress indexBufferAddress = GetBufferDeviceAddress(context, indexBuffer.buffer);

    // Specify where the builder can find the vertices and indices for triangles, and their formats:
    VkAccelerationStructureGeometryTrianglesDataKHR triangles = nvvk::make<VkAccelerationStructureGeometryTrianglesDataKHR>();
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = vertexBufferAddress;
    triangles.vertexStride = 3 * sizeof(float);
    triangles.maxVertex = static_cast<uint32_t>(objVertices.size() - 1);
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = indexBufferAddress;
    triangles.transformData.deviceAddress = 0;  // No transform (Identity matrix)

    // Create a VkAccelerationStructureGeometryKHR object that says it handles opaque triangles and points
    VkAccelerationStructureGeometryKHR geometryInfo = nvvk::make<VkAccelerationStructureGeometryKHR>();
    geometryInfo.geometry.triangles = triangles;
    geometryInfo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometryInfo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    blas.asGeometry.push_back(geometryInfo);

    // Create offset info that allows us to say how many triangles and vertices to read
    VkAccelerationStructureBuildRangeInfoKHR offsetInfo;
    offsetInfo.firstVertex = 0;
    offsetInfo.primitiveCount = static_cast<uint32_t>(objIndices.size() / 3);  // Number of triangles
    offsetInfo.primitiveOffset = 0;
    offsetInfo.transformOffset = 0;
    blas.asBuildOffsetInfo.push_back(offsetInfo);
    blases.push_back(blas);
  }

  // Create the BLAS (nvvk::RaytracingBuilderKHR creates, records, ends, submits, and waits for command buffers, indicating that CPU thread waits until GPU is done building)
  nvvk::RaytracingBuilderKHR raytracingBuilder;
  raytracingBuilder.setup(context, &allocator, context.m_queueGCT);
  raytracingBuilder.buildBlas(blases, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR);

  // Create an instance pointing to this BLAS, and build it into a TLAS
  std::vector<nvvk::RaytracingBuilderKHR::Instance> instances;
  {
    nvvk::RaytracingBuilderKHR::Instance instance;
    instance.transform.identity();  // Set the instance transform to the identity matrix
    instance.instanceCustomId = 0;  // 24 bits accessible to ray shaders via rayQueryGetIntersectionInstanceCustomIndexEXT
    instance.blasId = 0;  // The index of the BLAS in `blases` that this instance points to
    instance.hitGroupId = 0;  // Used for a shader offset index, accessible via rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;  // How to trace this instance
    instances.push_back(instance);
  }

  raytracingBuilder.buildTlas(instances, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);

  // Here's the list of bindings for the descriptor set layout, from raytrace.comp.glsl:
  // 0 - a storage buffer (the buffer `buffer`)
  // 1 - an acceleration structure (the TLAS)
  nvvk::DescriptorSetContainer descriptorSetContainer(context);
  descriptorSetContainer.addBinding(BINDING_IMAGEDATA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
  descriptorSetContainer.addBinding(BINDING_TLAS, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT);
  descriptorSetContainer.addBinding(BINDING_VERTICES, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
  descriptorSetContainer.addBinding(BINDING_INDICES, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);

  // Create a layout from the list of bindings
  descriptorSetContainer.initLayout();

  // Create a descriptor pool from the list of bindings with space for one set, and all the set
  descriptorSetContainer.initPool(1);
  // Create a simple pipeline layout from the descriptor set layout
  descriptorSetContainer.initPipeLayout();

  // Write values into the descriptor set.
  std::array<VkWriteDescriptorSet, 4> writeDescriptorSets;
  // 0
  VkDescriptorBufferInfo descriptorBufferInfo{};
  descriptorBufferInfo.buffer = buffer.buffer;    // The VkBuffer object
  descriptorBufferInfo.range = bufferSizeBytes;  // The length of memory to bind; offset is 0.
  writeDescriptorSets[0] = descriptorSetContainer.makeWrite(0 /*set index*/, BINDING_IMAGEDATA /*binding*/, &descriptorBufferInfo);
  // 1
  VkWriteDescriptorSetAccelerationStructureKHR descriptorAS = nvvk::make<VkWriteDescriptorSetAccelerationStructureKHR>();
  VkAccelerationStructureKHR tlasCopy = raytracingBuilder.getAccelerationStructure();  // So that we can take its address
  descriptorAS.accelerationStructureCount = 1;
  descriptorAS.pAccelerationStructures = &tlasCopy;
  writeDescriptorSets[1] = descriptorSetContainer.makeWrite(0, BINDING_TLAS, &descriptorAS);
  // 2
  VkDescriptorBufferInfo vertexDescriptorBufferInfo{};
  vertexDescriptorBufferInfo.buffer = vertexBuffer.buffer;
  vertexDescriptorBufferInfo.range = VK_WHOLE_SIZE;
  writeDescriptorSets[2] = descriptorSetContainer.makeWrite(0, BINDING_VERTICES, &vertexDescriptorBufferInfo);
  // 3
  VkDescriptorBufferInfo indexDescriptorBufferInfo{};
  indexDescriptorBufferInfo.buffer = indexBuffer.buffer;
  indexDescriptorBufferInfo.range = VK_WHOLE_SIZE;
  writeDescriptorSets[3] = descriptorSetContainer.makeWrite(0, BINDING_INDICES, &indexDescriptorBufferInfo);
  vkUpdateDescriptorSets(context,                                            // The context
      static_cast<uint32_t>(writeDescriptorSets.size()),  // Number of VkWriteDescriptorSet objects
      writeDescriptorSets.data(),                         // Pointer to VkWriteDescriptorSet objects
      0, nullptr);  // An array of VkCopyDescriptorSet objects (unused)

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

  // Create and start recording a command buffer
  VkCommandBuffer cmdBuffer = AllocateAndBeginOneTimeCommandBuffer(context, cmdPool);

  // Bind the compute pipeline
  vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);

  // Bind the descriptor set
  VkDescriptorSet descriptorSet = descriptorSetContainer.getSet(0);
  vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, descriptorSetContainer.getPipeLayout(), 0, 1, &descriptorSet, 0, nullptr);

  // Run the compute shader with enough workgroups to cover the entire buffer:
  vkCmdDispatch(cmdBuffer, (uint32_t(RENDER_WIDTH) + WORKGROUP_WIDTH - 1) / WORKGROUP_WIDTH, (uint32_t(RENDER_HEIGHT) + WORKGROUP_HEIGHT - 1) / WORKGROUP_HEIGHT, 1);

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

  // End and submit the command buffer, and then wait for it to finish with a vkQueueWaitIdle
  EndSubmitWaitAndFreeCommandBuffer(context, context.m_queueGCT, cmdPool, cmdBuffer);

  // Get the image data back from the GPU
  void* data = allocator.map(buffer);
  stbi_write_hdr("out.hdr", RENDER_WIDTH, RENDER_HEIGHT, 3, reinterpret_cast<float*>(data));
  allocator.unmap(buffer);

  vkDestroyPipeline(context ,computePipeline, nullptr);
  vkDestroyShaderModule(context, rayTraceModule, nullptr);
  descriptorSetContainer.deinit();
  raytracingBuilder.destroy();
  allocator.destroy(vertexBuffer);
  allocator.destroy(indexBuffer);
  vkDestroyCommandPool(context, cmdPool, nullptr);
  allocator.destroy(buffer);
  allocator.deinit();
  context.deinit();
}