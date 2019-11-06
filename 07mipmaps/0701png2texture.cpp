/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This tool converts a PNG to a texture. It also dumps out each mip level to
 * demonstrate the conversion steps (if the dump is enabled).
 */

#include <src/gn/vendor/skia/skiaglue.h>
#include <src/memory/memory.h>
#include <src/science/science.h>

#include <algorithm>

#include "../src/base_application.h"

const char* imgFilename = nullptr;
const char* outFilename = nullptr;
const char* checkFilename = nullptr;

namespace example {

// PNG2texture uses similar headless code to Sample 3.
struct PNG2texture : public BaseApplication {
  PNG2texture(language::Instance& inst) : BaseApplication(inst) {}

  memory::Stage stage{cpool, memory::ASSUME_POOL_QINDEX};

  int checkOutput(science::SmartCommandBuffer& buffer, memory::Image& mipmap,
                  memory::Image& combo) {
    // Copy from mipmap to combo, reducing down to 1 mip level.
    combo.info = mipmap.info;
    combo.info.mipLevels = 1;
    combo.info.usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (combo.ctorAndBindDeviceLocal()) {
      logE("combo.ctorAndBindDeviceLocal failed\n");
      return 1;
    }

    // Copy mipmap to combo. First combo gets mipmap's mip level 0. Then that
    // is overwritten the scaled-down mip levels to make a diagonal combo.
    std::vector<VkImageCopy> copies(1);
    VkImageCopy& copy0 = copies.back();
    copy0.srcSubresource = mipmap.getSubresourceLayers(0 /*mip level*/);
    copy0.dstSubresource = combo.getSubresourceLayers(0 /*mip level*/);
    copy0.srcSubresource.aspectMask &= copy0.dstSubresource.aspectMask;
    copy0.dstSubresource.aspectMask &= copy0.srcSubresource.aspectMask;

    copy0.srcOffset = {0, 0, 0};
    copy0.dstOffset = {0, 0, 0};

    copy0.extent = mipmap.info.extent;

    for (uint32_t mip = 1; mip < mipmap.info.mipLevels; mip++) {
      copies.emplace_back();
      VkImageCopy& copy1 = copies.back();
      copy1.srcSubresource = mipmap.getSubresourceLayers(mip);
      copy1.dstSubresource = combo.getSubresourceLayers(0);
      copy1.srcSubresource.aspectMask &= copy1.dstSubresource.aspectMask;
      copy1.dstSubresource.aspectMask &= copy1.srcSubresource.aspectMask;

      copy1.srcOffset = {0, 0, 0};
      copy1.dstOffset = {int32_t(combo.info.extent.width >> mip),
                         int32_t(combo.info.extent.height >> mip), 0};
      copy1.extent = mipmap.info.extent;
      copy1.extent.width >>= mip;
      copy1.extent.height >>= mip;
    }

    if (buffer.barrier(mipmap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) ||
        buffer.barrier(combo, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) ||
        buffer.copyImage(mipmap, combo, copies)) {
      logE("barriers or copyImage failed\n");
      return 1;
    }
    return 0;
  }

  int run() {
    if (cpool.ctorError()) {
      return 1;
    }

    // skiaglue sets image format and image extent from file.
    memory::Image mipmap(cpool.vk.dev);
    if (mipmap.setName("mipmap")) {
      logE("mipmap.setName failed\n");
      return 1;
    }
    std::shared_ptr<memory::Flight> flight;
    skiaglue skGlue;
    if (skGlue.loadImage(imgFilename, stage, flight, mipmap)) {
      logE("loadImage(%s) failed\n", imgFilename);
      return 1;
    }
    mipmap.info.usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    language::ImageFormatProperties formatProps;
    VkResult v = formatProps.getProperties(cpool.vk.dev, mipmap.info);
    if (v != VK_SUCCESS) {
      logE("ImageFormatProperties::getProperties failed: %d %s\n", v,
           string_VkResult(v));
      if (v == VK_ERROR_FORMAT_NOT_SUPPORTED) {
        logE(
            "FormatProperties but no ImageFormatProperties for tiling %s "
            "format %s\n",
            string_VkImageTiling(mipmap.info.tiling),
            string_VkFormat(mipmap.info.format));
      }
      return 1;
    }
    auto& maxE = formatProps.imageFormatProperties.maxExtent;
    logI("format %s: maxExtent x=%llx y=%llx z=%llx\n",
         string_VkFormat(mipmap.info.format), (unsigned long long)maxE.width,
         (unsigned long long)maxE.height, (unsigned long long)maxE.depth);
    logI("    maxMipLevels=%u\n",
         formatProps.imageFormatProperties.maxMipLevels);
    logI("    maxArrayLayers=%u\n",
         formatProps.imageFormatProperties.maxArrayLayers);
    logI("    sampleCounts (flags) = %x\n",
         formatProps.imageFormatProperties.sampleCounts);
    logI("    maxResourceSize=%llx\n",
         (unsigned long long)formatProps.imageFormatProperties.maxResourceSize);
    // mipmap.info.usage was set before ImageFormatProperties, it needs it too.
    if (mipmap.setMipLevelsFromExtent() || mipmap.ctorAndBindDeviceLocal()) {
      logE("mipmap.ctorAndBindDeviceLocal failed\n");
      return 1;
    }

    // Flush flight.
    if (stage.flushAndWait(flight)) {
      logE("stage.flushAndWait(flight) failed\n");
      return 1;
    }
    flight.reset();

    // combo is only allocated and used if checkFilename is not NULL.
    memory::Image combo(cpool.vk.dev);
    if (combo.setName("combo")) {
      logE("combo.setName failed\n");
      return 1;
    }

    // Generate mipmap.
    // NOTE: it would be more efficient to batch up all commands and only do
    // a single submit. The code would have to fall back to a SmartCommandBuffer
    // if (flight->canSubmit() == false). Since that would be more complex, this
    // just sticks to the slower code path which always does a separate submit.
    {
      science::SmartCommandBuffer buffer{cpool, stage.poolQindex};
      if (buffer.ctorError() || buffer.autoSubmit() ||
          science::copyImageToMipmap(buffer, mipmap)) {
        logE("generateMipMap: copy or blit failed\n");
        return 1;
      }
      // At least share the SmartCommandBuffer with checkOutput(). Then these
      // only result in one submit between them.
      if (checkFilename) {
        if (checkOutput(buffer, mipmap, combo)) {
          return 1;
        }
      }
    }
    // When SmartCommandBuffer is destroyed it auto-submits. Now combo is done.
    if (checkFilename) {
      if (skGlue.writeToFile(combo, stage, checkFilename)) {
        return 1;
      }
      logI("wrote: %s\n", checkFilename);
    }

    return skGlue.writeToFile(mipmap, stage, outFilename);
  }
};

int headlessMain() {
  language::Instance instance;
  if (!instance.minSurfaceSupport.erase(language::PRESENT)) {
    logE("removing PRESENT from minSurfaceSupport: not found\n");
    return 1;
  }
  if (instance.ctorError(
          [](language::Instance&, void*) -> VkResult { return VK_SUCCESS; },
          nullptr) ||
      // Set Vulkan frame buffer (image) size to 800 x 600.
      // Frame buffer is 800 x 600, but the output extent matches its input
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

  int r = std::make_shared<PNG2texture>(instance)->run();
  return r;
}

}  // namespace example

// OS-specific startup code.
#ifdef _WIN32
// Windows-specific startup.
int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  if (__argc != 3 && __argc != 4) {
    OutputDebugString("This program takes 2 arguments.");
    fprintf(stderr,
            "Usage: %s input output\nConverts .png to .dds format or .ktx.\n",
            __argv[0]);
    return 1;
  }
  // TODO: support wide characters by using __wargv[1], [2].
  imgFilename = __argv[1];
  outFilename = __argv[2];
  if (__argc == 4) {
    checkFilename = __argv[3];
  }
  return example::headlessMain();
}
#elif defined(__ANDROID__)
// Android: technically it *could* run, but this is an opportunity to show how
// Volcano can be used for asset generation (your "asset pipeline") as well as
// the GUI client.
#error This tool is used for building image assets. It does not run on android.
#else
// Posix startup.
int main(int argc, char** argv) {
  if (argc != 3 && argc != 4) {
    fprintf(stderr,
            "Usage: %s input output\nConverts .png to .dds format or .ktx.\n",
            argv[0]);
    return 1;
  }
  imgFilename = argv[1];
  outFilename = argv[2];
  if (argc == 4) {
    checkFilename = argv[3];
  }
  return example::headlessMain();
}
#endif
