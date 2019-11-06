/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 */

#include <stdint.h>

#include "02sdl/02sdl.frag.h"
#include "02sdl/02sdl.vert.h"
#include "sdlglue.h"

namespace example {

// Example02 shows the steps to get to "first triangle."
class Example02 : public BaseApplication {
 public:
  // pipe0 is needed in run and onResizeRequest, so it is declared here.
  std::shared_ptr<command::Pipeline> pipe0;
  // cmdBuffers are needed in run and onResizeRequest, so declare them here.
  std::vector<command::CommandBuffer> cmdBuffers;
  // surface is needed in run and onResized, so declared it as a class member.
  VkSurfaceKHR surface;

  Example02(language::Instance& instance)
      : BaseApplication{instance},
        // Construct a Pipeline for the BaseApplication::RenderPass.
        pipe0{pass.addPipeline()} {
    // Register onResizeFramebuf in CommandPoolContainer.
    resizeFramebufListeners.emplace_back(std::make_pair(
        [](void* self, language::Framebuf& framebuf, size_t fbi,
           size_t /*poolQindex*/) -> int {
          return static_cast<Example01*>(self)->onResizeFramebuf(framebuf, fbi);
        },
        this));
  }

  int onResizeFramebuf(language::Framebuf& framebuf, size_t framebuf_i) {
    // Allocate the VkCommandBuffer handles. Only done for the first framebuf.
    if (framebuf_i == 0 && cpool.reallocCmdBufs(cmdBuffers, pass, 0)) {
      return 1;
    }

    // Patch viewport.
    auto& newSize = cpool.dev.swapChainInfo.imageExtent;
    VkViewport& viewport = pipe0->info.viewports.at(0);
    viewport.width = (float)newSize.width;
    viewport.height = (float)newSize.height;

    // Patch scissors.
    pipe0->info.scissors.at(0).extent = newSize;

    auto& cmdBuffer = cmdBuffers.at(framebuf_i);
    if (cmdBuffer.beginSimultaneousUse() ||
        // Begin RenderPass. There is only one subpass, i.e. subpass 0.
        cmdBuffer.beginSubpass(pass, framebuf, 0) ||
        cmdBuffer.bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipe) ||
        cmdBuffer.setViewport(pass) || cmdBuffer.setScissor(pass) ||
        cmdBuffer.draw(3, 1, 0, 0) ||
        // End RenderPass.
        cmdBuffer.endRenderPass() || cmdBuffer.end()) {
      logE("onResizeFramebuf: cmdBuffer [%zu] failed\n", framebuf_i);
      return 1;
    }
    return 0;
  }

  // run implements BaseApplication::run and executes the main loop.
  virtual int run() {
    // CommandPool cpool maintains a public Device &dev because a CommandPool
    // is just a block of RAM on the device. An instance of a CommandPool must
    // always be able to find the device it is living on.
    //
    // Leverage that to pass the same Device to everything.
    language::Device& dev = cpool.dev;

    pipe0->info.dynamicStates.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
    pipe0->info.dynamicStates.emplace_back(VK_DYNAMIC_STATE_SCISSOR);

    command::Semaphore imageAvailableSemaphore(dev);
    command::PresentSemaphore renderSemaphore(dev);
    auto vshader = std::make_shared<command::Shader>(dev);
    auto fshader = std::make_shared<command::Shader>(dev);

    if (cpool.ctorError() || imageAvailableSemaphore.ctorError(dev) ||
        renderSemaphore.ctorError() ||
        // Read compiled-in shaders from app into Vulkan.
        vshader->loadSPV(spv_02sdl_vert, sizeof(spv_02sdl_vert)) ||
        fshader->loadSPV(spv_02sdl_frag, sizeof(spv_02sdl_frag)) ||
        // Add VkShaderModule objects to pipeline.
        pipe0->info.addShader(vshader, pass, VK_SHADER_STAGE_VERTEX_BIT) ||
        pipe0->info.addShader(fshader, pass, VK_SHADER_STAGE_FRAGMENT_BIT) ||
        // Manually resize language::Framebuf the first time.
        onResized(dev.swapChainExtent)) {
      return 1;
    }

    // Begin main loop.
    unsigned frameCount = 0;
    float elapsedTime = 0;
    while (pollGlue(elapsedTime) == 0) {
      frameCount++;
      if (elapsedTime > 1.0) {
        logI("%d fps\n", frameCount);
        resetTimer();
        frameCount = 0;
      }

      uint32_t next_image_i;
      auto result = vkAcquireNextImageKHR(
          dev.dev, dev.swapChain, std::numeric_limits<uint64_t>::max(),
          imageAvailableSemaphore.vk, VK_NULL_HANDLE, &next_image_i);
      switch (result) {
        case VK_SUCCESS:
        case VK_SUBOPTIMAL_KHR:
          // Submit, present, and wait for the queue to be idle.
          if (cmdBuffers.at(next_image_i)
                  .submit(0, {imageAvailableSemaphore.vk},
                          {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT},
                          {renderSemaphore.vk}) ||
              renderSemaphore.present(next_image_i) ||
              renderSemaphore.waitIdle()) {
            return 1;
          }
          break;

        case VK_ERROR_OUT_OF_DATE_KHR:
          onResized({dev.swapChainExtent.width, dev.swapChainExtent.height});
          break;

        default:
          logE("%s failed: %d (%s)\n", "vkAcquireNextImageKHR", v,
               string_VkResult(v));
          return 1;
      }
    }
    return cpool.deviceWaitIdle();
  }
};

// appFactory is called by GLFWglue to construct the application after GLFWglue
// has set up language::Instance.
static BaseApplication* appFactory(language::Instance& instance) {
  return new Example02(instance);
}

}  // namespace example

using namespace example;

int main() {
  SDLglue glue;
  return glue.ctorError(appFactory);
}
