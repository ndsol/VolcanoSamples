/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This tool converts an equirectangular spherical panorama to the six faces
 * of a cube.
 */

#include <src/gn/vendor/skia/skiaglue.h>

#include "../src/base_application.h"
#include "../src/scanlinedecoder.h"
#include "10cubemap/1001sphere2cube.frag.h"
#include "10cubemap/1001sphere2cube.vert.h"

namespace frag {
#include "10cubemap/struct_1001sphere2cube.frag.h"
}

const char* imgFilename = nullptr;
const char* outFilename = nullptr;
const char* checkFilename = nullptr;

SK_API void skDebugVPrinter(const char format[], va_list ap) {
  logVolcano('E', format, ap);
}

namespace example {

// Sphere2cube runs headless.
struct Sphere2cube : public BaseApplication {
  Sphere2cube(language::Instance& inst) : BaseApplication(inst) {}
  memory::Stage stage{cpool, memory::ASSUME_POOL_QINDEX};
  ScanlineDecoder codec{stage};
  skiaglue skGlue;
  std::shared_ptr<memory::Image> cubemap;

  int loadEquirectangular(const char* sourceFileName) {
    codec.sampler.info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    logI("reading %s ...\n", sourceFileName);
    if (codec.open(sourceFileName)) {
      return 1;
    }
    auto& imgSize = codec.sampler.image->info.extent;
    // Create a framebuffer twice as wide as the input image.
    cpool.vk.dev.swapChainInfo.imageExtent.width = imgSize.width * 2;
    cpool.vk.dev.swapChainInfo.imageExtent.height = imgSize.height;

    while (codec.moreLines()) {
      if (codec.read()) {
        logE("read failed at line %zu\n", (size_t)codec.cpu.lineCount);
        return 1;
      }
    }
    return 0;
  }

  int writeCubemap(int outputNum, int imgOffset) {
    if (!outFilename) {
      return 0;
    }

    VkBufferImageCopy copy0;
    memset(&copy0, 0, sizeof(copy0));
    copy0.bufferOffset = 0;
    copy0.imageSubresource = cubemap->getSubresourceLayers(0 /*mip level*/);

    // Use a custom imageOffset, imageExtent to write just a part of the image.
    copy0.imageExtent = cubemap->info.extent;
    copy0.imageExtent.width = cubemap->info.extent.width / 4;
    copy0.imageOffset = {int32_t(copy0.imageExtent.width) * imgOffset, 0, 0};
    copy0.bufferRowLength = copy0.imageExtent.width;
    copy0.bufferImageHeight = copy0.imageExtent.height;

    char filename[256];
    snprintf(filename, sizeof(filename), "%s%d.jpg", outFilename, outputNum);
    logI("writing %s ...\n", filename);
    if (skGlue.writeToFile(*cubemap, stage, filename, {copy0})) {
      return 1;
    }
    return 0;
  }

  int renderProjection(command::RenderPass& pass, science::PipeBuilder& pipe0,
                       memory::DescriptorSet& descriptorSet) {
    // Get sampler.img in SHADER_READ_ONLY - it is a texture in the shader.
    // (This just records the command to the VkCommandBuffer, but it also
    // updates codec.sampler.image.currentLayout as if the command had run.)
    science::SmartCommandBuffer cmd{cpool, stage.poolQindex};
    if (cmd.ctorError() || cmd.autoSubmit() || pass.ctorError() ||
        cmd.barrier(*codec.sampler.image,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
        // descriptorSet.write() uses codec.sampler.image->currentLayout, so be
        // careful to only call write() after the call to cmd.barrier().
        descriptorSet.write(frag::bindingIndexOftexSampler(),
                            std::vector<science::Sampler*>{&codec.sampler})) {
      logE("barrier, RenderPass, Framebuf, or descriptorSet.write failed\n");
      return 1;
    }

    // pipe0.info().attach describes the framebuf.attachments.
    // cmd.beginSubpass() will transition cubemap to a new layout. (see
    // definition of VkAttachmentDescription2KHR::finalLayout).
    // cubemap->currentLayout is just a Volcano state variable. It needs to be
    // updated manually to match what happens in the render pass:
    cubemap->currentLayout = pipe0.info().attach.at(0).vk.finalLayout;

    if (cmd.beginSubpass(pass, pass.getTargetFramebuf(), 0) ||
        cmd.bindGraphicsPipelineAndDescriptors(*pipe0.pipe, 0, 1,
                                               &descriptorSet.vk) ||
        cmd.setViewport(0, 1, &pipe0.info().viewports.at(0)) ||
        cmd.setScissor(0, 1, &pipe0.info().scissors.at(0)) ||
        cmd.draw(4, 1, 0, 0) ||
        // End RenderPass.
        cmd.endRenderPass()) {
      logE("renderProjection: command buffer failed.\n");
      return 1;
    }
    return 0;
  }

  // projectToCube converts an equirectangular projection (reading from
  // codec.sampler.image) to a cubemap.
  int projectToCube() {
    auto& dev = cpool.vk.dev;
    cubemap = std::make_shared<memory::Image>(dev);

    cubemap->info = codec.sampler.image->info;
    cubemap->info.format = dev.swapChainInfo.imageFormat;
    cubemap->info.extent.width = cpool.vk.dev.swapChainInfo.imageExtent.width;
    cubemap->info.extent.height = cpool.vk.dev.swapChainInfo.imageExtent.height;
    cubemap->info.mipLevels = 1;
    cubemap->info.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    // Specialization constants for fragment shader.
    // It would be a lot of code to rebuild the pipeline with updated constants
    // if this were part of a real-time pipeline, but fortunately there is no
    // "the user resized the window" event. This is a headless app.
    frag::SpecializationConstants spec;
    spec.OUT_W = (float)cpool.vk.dev.swapChainInfo.imageExtent.width;
    spec.OUT_H = (float)cpool.vk.dev.swapChainInfo.imageExtent.height;
    spec.SAMPLER_W = codec.sampler.image->info.extent.width;
    spec.SAMPLER_H = codec.sampler.image->info.extent.height;
    // Set up RenderPass for its first run-through (with SIDES==true)
    spec.SIDES = true;

    auto vert = std::make_shared<command::Shader>(dev);
    auto frag = std::make_shared<command::Shader>(dev);
    science::ShaderLibrary shaders{cpool.vk.dev};

    // Read compiled-in shaders from app into Vulkan.
    if (vert->loadSPV(spv_1001sphere2cube_vert,
                      sizeof(spv_1001sphere2cube_vert)) ||
        frag->loadSPV(spv_1001sphere2cube_frag,
                      sizeof(spv_1001sphere2cube_frag)) ||
        cubemap->ctorAndBindDeviceLocal()) {
      logE("projectToCube: failed to load shaders\n");
      return 1;
    }

    command::RenderPass pass(dev);
    science::PipeBuilder pipe0{pass};
    pipe0.info().dynamicStates.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
    pipe0.info().dynamicStates.emplace_back(VK_DYNAMIC_STATE_SCISSOR);

    VkViewport& viewport = pipe0.info().viewports.at(0);
    viewport.width = spec.OUT_W;
    viewport.height = spec.OUT_H;

    pipe0.info().scissors.at(0).extent = cpool.vk.dev.swapChainInfo.imageExtent;
    // Instead of 6 vertices to produce 2 triangles in a rect, just use 4:
    pipe0.info().asci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    science::DescriptorLibrary descriptorLibrary{cpool.vk.dev};
    if (shaders.add(pipe0, vert) || shaders.add(pipe0, frag) ||
        shaders.finalizeDescriptorLibrary(descriptorLibrary) ||
        pipe0.info().specialize(spec) || pass.setTargetImage(cubemap)) {
      logE("shaders or descriptorLibrary failed\n");
      return 1;
    }

    auto descriptorSet = descriptorLibrary.makeSet(0);
    if (!descriptorSet) {
      logE("descriptorLibrary.makeSet failed\n");
      return 1;
    }

    if (renderProjection(pass, pipe0, *descriptorSet)) {
      logE("first renderProjection failed\n");
      return 1;
    }
    if (writeCubemap(0, 2) ||  // +X face (matches OpenGL order)
        writeCubemap(1, 0) ||  // -X face
        writeCubemap(4, 1) ||  // +Z face
        writeCubemap(5, 3)) {  // -Z face
      return 1;
    }

    // Update RenderPass: Set SIDES = false and re-build pass.
    spec.SIDES = false;
    if (pipe0.info().specialize(spec)) {
      logE("second specialize failed\n");
      return 1;
    }

    // Reset VkRenderPass. projectToCubeCommands then calls pass.ctorError.
    pass.vk.reset();
    return renderProjection(pass, pipe0, *descriptorSet) ||
           writeCubemap(2, 0) ||  // +Y face (top)
           writeCubemap(3, 2);    // -Y face (bottom)
  }

  int run() {
    if (cpool.ctorError()) {
      return 1;
    }

    if (loadEquirectangular(imgFilename)) {
      return 1;
    }

    if (projectToCube()) {
      return 1;
    }

    if (cpool.deviceWaitIdle()) {
      logE("cpool.deviceWaitIdle (at very end) failed\n");
      return 1;
    }
    return 0;
  }
};

int headlessMain() {
  gSkDebugVPrinter = skDebugVPrinter;
  language::Instance instance;
  if (!instance.minSurfaceSupport.erase(language::PRESENT)) {
    logE("removing PRESENT from minSurfaceSupport: not found\n");
    return 1;
  }
  if (instance.ctorError(
          [](language::Instance&, void*) -> VkResult { return VK_SUCCESS; },
          nullptr) ||
      // Set a frame buffer size here. Change it above.
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

  // Set the image format so the pipeline automatically gets the right format.
  dev.swapChainInfo.imageFormat = dev.chooseFormat(
      VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
      VK_IMAGE_TYPE_2D, {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UINT});
  if (dev.swapChainInfo.imageFormat == VK_FORMAT_UNDEFINED) {
    logE("chooseFormat(R8G8B8A8_{SRGB,UINT}) failed\n");
    return 1;
  }
  int r = std::make_shared<Sphere2cube>(instance)->run();
  return r;
}

}  // namespace example

// OS-specific startup code.
#ifdef _WIN32
// Windows-specific startup.
int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  if (__argc != 3 && __argc != 4) {
    OutputDebugString("This program takes 2 arguments.");
    fprintf(stderr, "Usage: %s input output\nConverts sphere to cube map.\n",
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
    fprintf(stderr, "Usage: %s input output\nConverts sphere to cube map.\n",
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
