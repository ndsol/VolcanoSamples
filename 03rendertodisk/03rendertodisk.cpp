/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This sample demonstrates avoiding VK_KHR_swapchain by writing to disk.
 */

#include <errno.h>
#include <src/memory/memory.h>
#include <src/science/science.h>
#include <stdint.h>
#include <string.h>

#include <sstream>

#include "../src/base_application.h"
#include "03rendertodisk/03rendertodisk.frag.h"
#include "03rendertodisk/03rendertodisk.vert.h"

namespace example {

/*
 * This writes a BMP file. This is neither interesting nor valuable, but adding
 * a dependency on Skia just to write one .BMP is too much.
 *
 * ========= Begin one very short BMP library =========
 */
#pragma pack(push, 1)
struct BMPHeader {
  char magic[2] = {'B', 'M'};
  uint32_t filesize;
  uint16_t reserved[2] = {0, 0};
  uint32_t dataOffset = sizeof(BMPHeader);

  struct BMPInfoHeader {
    uint32_t headerSize = sizeof(BMPInfoHeader);
    uint32_t width, height;
    uint16_t numColorPlanes = 1;
    uint16_t numBitsPerPixel = 24;
    uint32_t compressionMethod = 0;
    uint32_t compressedSize = 0;
    uint32_t horzResolution = 0;
    uint32_t vertResolution = 0;
    uint32_t colorPaletteSize = 0;
    uint32_t colorPaletteUsed = 0;
  } info;
};
#pragma pack(pop)

#ifndef _WIN32
// Force a compiler error if sizeof(BMPHeader) is not CORRECT_BMP_HEADER:
#define CORRECT_BMP_HEADER (54)
static int compiler_check_1[sizeof(BMPHeader) - CORRECT_BMP_HEADER];
static int compiler_check_2[CORRECT_BMP_HEADER - sizeof(BMPHeader)];
#endif

static int writeBMPbytes(const std::string filename, const memory::Image& img,
                         uint8_t* pixels, VkDeviceSize rowPitch, FILE* bmp) {
#ifndef _WIN32
  (void)compiler_check_1;
  (void)compiler_check_2;
#endif
  BMPHeader header;
  uint32_t bmpStride = (3 * img.info.extent.width + 3) & (~3);
  header.filesize = sizeof(header) + bmpStride * img.info.extent.height;
  header.info.width = img.info.extent.width;
  header.info.height = img.info.extent.height;
  if (fwrite(&header, 1, sizeof(header), bmp) != sizeof(header)) {
    logE("fwrite(%s, header): %d %s\n", filename.c_str(), errno,
         strerror(errno));
    return 1;
  }
  uint8_t* row = new uint8_t[bmpStride];
  for (uint32_t y = 0; y < img.info.extent.height; y++) {
    memset(row, 0, bmpStride);
    auto* pixel = row;
    // stackoverflow.com/questions/8346115/why-are-bmps-stored-upside-down
    auto* src = &pixels[(img.info.extent.height - 1 - y) * rowPitch];
    for (uint32_t x = 0; x < img.info.extent.width; x++, src += 4) {
      *(pixel++) = src[2];  // Blue channel (in other words, BGR format)
      *(pixel++) = src[1];
      *(pixel++) = src[0];  // Red channel
    }
    if (fwrite(row, 1, bmpStride, bmp) != bmpStride) {
      logE("fwrite(%s, row %u): %d %s\n", filename.c_str(), y, errno,
           strerror(errno));
      delete[] row;
      return 1;
    }
  }
  delete[] row;
  return 0;
}

static int writeBMP(const std::string filename, const memory::Image& img,
                    void* mem, VkDeviceSize pitch) {
  FILE* bmp = fopen(filename.c_str(), "wb");
  if (!bmp) {
    logE("fopen(%s, wb): %d %s\n", filename.c_str(), errno, strerror(errno));
    return 1;
  }
  if (writeBMPbytes(filename, img, static_cast<uint8_t*>(mem), pitch, bmp)) {
    fclose(bmp);
    return 1;
  }
  if (fclose(bmp)) {
    logE("fclose(%s): %d %s\n", filename.c_str(), errno, strerror(errno));
    return 1;
  }
  logI("wrote \"%s\"\n", filename.c_str());
  return 0;
}

// ========= End the very short BMP library =========

// example03 is documented at github.com/ndsol/VolcanoSamples/03rendertodisk/
int example03(language::Device& dev, VkFormat format, VkExtent2D extent) {
  command::RenderPass pass(dev);
  command::CommandPool cpool(dev);
  cpool.queueFamily = language::GRAPHICS;
  // image is the target to be rendered to on the device.
  auto image = std::make_shared<memory::Image>(dev);
  // Set image extent from swapChainInfo extent.
  image->info.extent = VkExtent3D{extent.width, extent.height, 1};
  image->info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image->info.format = format;
  image->info.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (pass.setTargetImage(image)) {
    logE("setTargetImage failed\n");
    return 1;
  }

  // host is to copy to "host visible" memory after rendering is done.
  memory::Buffer host{dev};
  host.info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkDeviceSize rowPitch = 0;
  switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UINT:
      rowPitch = 4 * extent.width;
      break;
    default:
      logE("format sizes can be calculated using gli, see assimpglue.cpp\n");
      return 1;
  }
  host.info.size = rowPitch * extent.height;

  std::shared_ptr<command::Pipeline> pipe0(pass.addPipeline());
  pipe0->info.dynamicStates.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
  pipe0->info.dynamicStates.emplace_back(VK_DYNAMIC_STATE_SCISSOR);

  VkViewport& viewport = pipe0->info.viewports.at(0);
  viewport.width = (float)extent.width;
  viewport.height = (float)extent.height;

  pipe0->info.scissors.at(0).extent = extent;

  auto vert = std::make_shared<command::Shader>(dev);
  auto frag = std::make_shared<command::Shader>(dev);
  std::vector<command::CommandBuffer> cmdBuffers;
  command::Fence renderDoneFence(dev);
  if (cpool.ctorError() || renderDoneFence.ctorError() ||
      image->ctorAndBindDeviceLocal() ||
      // Read compiled-in shaders from app into Vulkan.
      vert->loadSPV(spv_03rendertodisk_vert, sizeof(spv_03rendertodisk_vert)) ||
      frag->loadSPV(spv_03rendertodisk_frag, sizeof(spv_03rendertodisk_frag)) ||
      // Add VkShaderModule objects to pipeline.
      pipe0->info.addShader(pass, vert, VK_SHADER_STAGE_VERTEX_BIT) ||
      pipe0->info.addShader(pass, frag, VK_SHADER_STAGE_FRAGMENT_BIT) ||
      //  Build RenderPass.
      pass.ctorError() || host.ctorAndBindHostCoherent()) {
    logE("pass failed\n");
    return 1;
  }
  {
    std::vector<VkCommandBuffer> vk(1);
    if (cpool.alloc(vk)) {  // Allocate 1 new VkCommandBuffer handle.
      logE("cpool.alloc() failed\n");
      return 1;
    }
    cmdBuffers.emplace_back(cpool);
    cmdBuffers.at(0).vk = vk.at(0);
  }

  // pipe0->info.attach describes the framebuf.attachments.
  // cmdBuffer.beginSubpass() will transition image to a new layout (see
  // definition of VkAttachmentDescription2KHR::finalLayout).
  // image->currentLayout is just a Volcano state variable. It needs to be
  // updated manually to match what happens in the render pass:
  image->currentLayout = pipe0->info.attach.at(0).vk.finalLayout;

  VkBufferImageCopy copy0;
  memset(&copy0, 0, sizeof(copy0));
  copy0.bufferOffset = 0;
  copy0.bufferRowLength = image->info.extent.width;
  copy0.bufferImageHeight = image->info.extent.height;
  copy0.imageSubresource = image->getSubresourceLayers(0 /*mip level*/);
  copy0.imageOffset = {0, 0, 0};
  copy0.imageExtent = image->info.extent;
  command::CommandBuffer& cmdBuffer = cmdBuffers.at(0);
  if (cmdBuffer.beginSimultaneousUse() ||
      cmdBuffer.beginSubpass(pass, pass.getTargetFramebuf(), 0) ||
      cmdBuffer.bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe0) ||
      cmdBuffer.setViewport(0, 1, &viewport) ||
      cmdBuffer.setScissor(0, 1, &pipe0->info.scissors.at(0)) ||
      cmdBuffer.draw(3, 1, 0, 0) ||
      // End RenderPass.
      cmdBuffer.endRenderPass() ||
      // Copy from image to host
      cmdBuffer.barrier(*image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) ||
      cmdBuffer.copyImageToBuffer(image->vk, image->currentLayout, host.vk,
                                  {copy0}) ||
      cmdBuffer.end()) {
    logE("failed to build command buffer.\n");
    return 1;
  }

  // Begin main loop.
  void* hostMmap = 0;
  unsigned bmpCount = 0;
  // Terminate the app after lots of frames instead of after writing one
  // BMP file. This should help trigger any timing bugs around
  // synchronizing the host to the device on each frame.
  for (unsigned frameCount = 0; frameCount <= 1000; frameCount++) {
    // Submit the VkCommandBuffer to the device.
    if (cpool.submitAndWait(memory::ASSUME_POOL_QINDEX, cmdBuffers.at(0))) {
      logE("cpool.submitAndWait failed\n");
      return 1;
    }

    if (bmpCount < 1) {
      // Only mmap hostMmap once. Lazily unmap it when exiting.
      if (!hostMmap && host.mem.mmap(&hostMmap)) {
        logE("host.mem.mmap failed\n");
        return 1;
      }
      std::ostringstream filename;
      filename << "test" << bmpCount << ".bmp";
      if (writeBMP(filename.str(), *image, hostMmap, rowPitch)) {
        return 1;
      }
      bmpCount++;
    }
  }
  host.mem.munmap();
  return cpool.deviceWaitIdle();
}

}  // namespace example

using namespace example;

static VkResult emptySurfaceFn(language::Instance&, void* /*window*/) {
  return VK_SUCCESS;
}

// headlessMain: for when you chop off all the window system integration.
int headlessMain() {
  language::Instance instance;
  // Tell Volcano devices without PRESENT are ok. VK_KHR_swapchain is not
  // needed. Volcano will still enable VK_KHR_swapchain if present as a
  // convenience, but ignore that.
  if (!instance.minSurfaceSupport.erase(language::PRESENT)) {
    logE("removing PRESENT from minSurfaceSupport: not found\n");
    return 1;
  }
  if (instance.ctorError(emptySurfaceFn, nullptr) ||
      // Set frame buffer size to 800 x 600 - sets the final output size.
      instance.open({800, 600})) {
    logE("instance.ctorError or open failed\n");
    return 1;
  }
  if (!instance.devs.size()) {
    logE("No devices found. Does your device support Vulkan?\n");
    return 1;
  }

  // Remove auto-selected presentModes to prevent VK_KHR_swapchain being used
  // and device framebuffers being auto-created.
  auto& dev = *instance.devs.at(0);
  dev.presentModes.clear();

  // Set the image format for the pipeline.
  VkFormat format = dev.chooseFormat(
      VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
      VK_IMAGE_TYPE_2D, {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UINT});
  if (format == VK_FORMAT_UNDEFINED) {
    logE("chooseFormat(R8G8B8A8_{SRGB,UINT}) failed\n");
    return 1;
  }
  return example03(dev, format, dev.swapChainInfo.imageExtent);
}

// OS-specific startup code.
#ifdef _WIN32
// Windows-specific startup.
int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  return headlessMain();
}

#elif defined(__ANDROID__)
#error __ANDROID__ is not supported.

#else
// Posix startup.
int main() { return headlessMain(); }
#endif
