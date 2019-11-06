/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * Implements the Dear ImGui UI
 */

#include "20texture.h"
#include "imgui.h"

// glslangValidator generated SPIR-V bytecode:
#include "20compute/20texture.frag.h"
#include "20compute/20texture.vert.h"

// uniform_glue.h in 20texture.h has already #included some glm headers.
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace example {

#include "20compute/struct_20texture.vert.h"
namespace frag {
#include "20compute/struct_20texture.frag.h"
}

// A single oversize triangle can be faster than a fullscreen quad
const std::vector<st_20texture_vert> vertices = {
    {{0.f, 0.f, 0.0f}, {0.0f, 0.0f}},
    {{1.f, 0.f, 0.0f}, {1.0f, 0.0f}},
    {{1.f, 1.f, 0.0f}, {1.0f, 1.0f}}};

const std::vector<uint32_t> indices = {
    2,
    1,
    0,
};

int Compressor::buildPass() {
  uglue.indices = indices;
  auto vert =
      std::shared_ptr<command::Shader>(new command::Shader(cpool.vk.dev));
  auto frag =
      std::shared_ptr<command::Shader>(new command::Shader(cpool.vk.dev));

  // Construct sampler with the size of the input (inputW x inputH).
  sampler.info.anisotropyEnable = VK_TRUE;
  sampler.info.magFilter = VK_FILTER_LINEAR;
  sampler.info.minFilter = VK_FILTER_LINEAR;
  sampler.info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler.image->info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
  auto& info = sampler.image->info;
  info.format = VK_FORMAT_R8G8B8A8_SRGB;
  info.extent.width = textureGPU.inputW;
  info.extent.height = textureGPU.inputH;
  info.extent.depth = 1;
  if (sampler.image->setMipLevelsFromExtent()) {
    logE("buildPass: setMipLevelsFromExtent failed\n");
    return 1;
  }
  sampler.info.maxLod = info.mipLevels;
  sampler.imageView.info.subresourceRange.levelCount = info.mipLevels;

  VkDeviceSize imgSize = textureGPU.decoder.stride * textureGPU.inputH;

  if (sampler.ctorError()) {
    logE("buildPass: sampler.ctorError failed\n");
    return 1;
  }

  {  // flight and smart must go out of scope at the end of this block.
    std::shared_ptr<memory::Flight> flight;
    if (uglue.stage.mmap(*sampler.image, imgSize, flight)) {
      logE("buildPass: src.size=%zu stage.mmap failed\n", (size_t)imgSize);
      return 1;
    }
    memset(flight->mmap(), 0, imgSize);

    flight->copies.resize(1);
    VkBufferImageCopy& copy = flight->copies.at(0);
    copy.bufferOffset = 0;
    copy.bufferRowLength = info.extent.width;
    copy.bufferImageHeight = info.extent.height;
    copy.imageSubresource = sampler.image->getSubresourceLayers(0);
    copy.imageOffset = {0, 0, 0};
    copy.imageExtent = {copy.bufferRowLength, copy.bufferImageHeight, 1};
    if (uglue.stage.flushAndWait(flight)) {
      logE("buildPass: flushAndWait failed\n");
      return 1;
    }
    science::SmartCommandBuffer smart(cpool, uglue.stage.poolQindex);
    if (smart.ctorError() || smart.autoSubmit() ||
        science::copyImageToMipmap(smart, *sampler.image) ||
        smart.barrier(*sampler.image,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
      logE("barrier(SHADER_READ_ONLY) failed\n");
      return 1;
    }
  }

  return cpool.setName("cpool") || pass.setName("pass") ||
         vert->setName("vert") || frag->setName("frag") ||
         cpool.vk.dev.setName("cpool.vk.dev") ||
         cpool.vk.dev.setSurfaceName("inst.surface") ||
         cpool.vk.dev.swapChain.setName("cpool.vk.dev.swapChain") ||
         uglue.indexBuffer.setName("uglue.indexBuffer") ||
         uglue.vertexBuffer.setName("uglue.vertexBuffer") ||
         pipe0.setName("pipe0") ||
         pipe0.pipe->pipelineLayout.setName("pipe0 layout") ||
         uglue.renderSemaphore.setName("uglue.renderSemaphore") ||
         uglue.imageAvailableSemaphore.setName("imageAvailableSemaphore") ||
         uglue.renderDoneFence.setName("uglue.renderDoneFence") ||
         vert->loadSPV(spv_20texture_vert, sizeof(spv_20texture_vert)) ||
         frag->loadSPV(spv_20texture_frag, sizeof(spv_20texture_frag)) ||
         uglue.shaders.add(pipe0, vert) || uglue.shaders.add(pipe0, frag) ||
         uglue.initPipeBuilderFrom(pipe0, vertices) ||
         uglue.buildPassAndTriggerResize();
}

int Compressor::buildFramebuf(language::Framebuf& framebuf, size_t fbi) {
  char name[256];
  auto& ds = *uglue.descriptorSet.at(fbi);
  snprintf(name, sizeof(name), "uglue.descriptorSet[%zu]", fbi);
  if (ds.setName(name) || ds.write(frag::bindingIndexOftexSampler(),
                                   std::vector<science::Sampler*>{&sampler})) {
    logE("uglue.descriptorSet[%zu] failed\n", fbi);
    return 1;
  }

  auto& newSize = cpool.vk.dev.swapChainInfo.imageExtent;
  VkViewport& viewport = pipe0.info().viewports.at(0);
  viewport.width = (float)newSize.width;
  viewport.height = (float)newSize.height;
  pipe0.info().scissors.at(0).extent = newSize;
  VkBuffer vertBufs[] = {uglue.vertexBuffer.vk};
  VkDeviceSize offsets[] = {0};

  auto& cmdBuffer = uglue.cmdBuffers.at(fbi);
  if (cmdBuffer.beginSimultaneousUse() ||
      cmdBuffer.beginSubpass(pass, framebuf, 0) ||
      cmdBuffer.bindGraphicsPipelineAndDescriptors(*pipe0.pipe, 0, 1, &ds.vk) ||
      cmdBuffer.setViewport(0, 1, &viewport) ||
      cmdBuffer.setScissor(0, 1, &pipe0.info().scissors.at(0)) ||
      cmdBuffer.bindVertexBuffers(0, sizeof(vertBufs) / sizeof(vertBufs[0]),
                                  vertBufs, offsets) ||
      cmdBuffer.bindAndDraw(uglue.indices, uglue.indexBuffer.vk,
                            0 /*indexBufOffset*/)) {
    logE("buildFramebuf: command buffer [%zu] failed\n", fbi);
    return 1;
  }
  return uglue.endRenderPass(cmdBuffer, fbi);
}

int Compressor::redraw(std::shared_ptr<memory::Flight>& flight) {
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(64, 64), ImGuiCond_FirstUseEver);
  ImGui::Begin("Config");
  if (ImGui::Button("Show Results")) {
    logE("TODO: if buffer is shared between graphics queue and compute,");
    logE("TODO: Buffer::ctorError(queufams)\n");
  }
  ImGui::End();

  auto& ubo = *reinterpret_cast<UniformBufferObject*>(flight->mmap());
  ubo.model = glm::rotate(
      glm::rotate(glm::mat4(1.0f), glm::radians(40.0f), glm::vec3(0.0f, 1, 0)),
      uglue.elapsed.get() * glm::radians(90.0f), glm::vec3(0.0f, 0, 1));

  ubo.view = glm::lookAt(glm::vec3(2.0f, 0, 0),      // Object pose.
                         glm::vec3(0.0f, 0, -0.23),  // Camera pose.
                         glm::vec3(0.0f, 0, 1));     // Up vector.

  ubo.proj = glm::perspective(glm::radians(45.0f), cpool.vk.dev.aspectRatio(),
                              0.1f, 10.0f);
  // Convert from OpenGL to Vulkan clip coordinates (+Y is down, not up).
  ubo.proj[1][1] *= -1;

  command::SubmitInfo info;
  if (uglue.submit(flight, info)) {
    logE("uglue.submit failed\n");
    return 1;
  }
  return 0;
}

int Compressor::initUI() {
  if (!uglue.window) {  // Initialize for console-only use.
    cpool.vk.dev.presentModes.clear();
    return 0;
  }

  // Customize Dear ImGui with a larger font size.
  uglue.fonts.at(0)->SizePixels = 18.f;
  if (cpool.ctorError() || uglue.imGuiInit() || buildPass() ||
      uglue.descriptorLibrary.setName("uglue.descriptorLibrary")) {
    return 1;
  }
  return 0;
}

}  // namespace example
