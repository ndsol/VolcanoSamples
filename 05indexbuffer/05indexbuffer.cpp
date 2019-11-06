/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This sample demonstrates both a vertex buffer and index buffer. By shader
 * reflection the app is statically type checked against the shader source.
 */

#include "../src/base_application.h"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#ifdef __ANDROID__
#include <unistd.h> /*for usleep()*/
#endif              /*__ANDROID__*/

#include "05indexbuffer/05indexbuffer.frag.h"
#include "05indexbuffer/05indexbuffer.vert.h"

namespace example {

// Shader reflection produces st_05indexbuffer_vert. This validates the
// vertices defined here against "inPosition" and "inColor" defined in the
// shader. The compiler can check that the vertices here match the expected
// vertex attributes in the shader.
//
// Shader reflection also produces UniformBufferObject
// (the type definition of the uniform buffer).
#include "05indexbuffer/struct_05indexbuffer.vert.h"

const std::vector<st_05indexbuffer_vert> vertices = {
    {{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}},
    {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}},

    {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}}};

const std::vector<uint16_t> indices = {
    0, 1, 2,  // Triangle 1 uses vertices[0] - vertices[2]
    2, 3, 0,  // Triangle 2 uses vertices[0] - vertices[3]
    4, 5, 6,  // Triangle 3 uses vertices[4] - vertices[6]
    6, 7, 4,  // Triangle 4 uses vertices[4] - vertices[7]
};

// Example05 is documented at github.com/ndsol/VolcanoSamples/05indexbuffer/
class Example05 : public BaseApplication {
 public:
  Example05(language::Instance& instance) : BaseApplication{instance} {
    resizeFramebufListeners.emplace_back(std::make_pair(
        [](void* self, language::Framebuf& framebuf, size_t fbi,
           size_t) -> int {
          return static_cast<Example05*>(self)->buildFramebuf(framebuf, fbi);
        },
        this));
  }

 protected:
  command::Semaphore renderSemaphore{cpool.vk.dev};
  VkQueue presentQueue{VK_NULL_HANDLE};
  std::vector<command::CommandBuffer> cmdBuffers;
  // shaders is a ShaderLibrary tracking all the shaders the app has loaded.
  science::ShaderLibrary shaders{cpool.vk.dev};
  // descriptorLibrary is the final output of the ShaderLibrary.
  science::DescriptorLibrary descriptorLibrary{cpool.vk.dev};
  // Use a memory::Stage to automate transferring data from host to GPU.
  memory::Stage stage{cpool, memory::ASSUME_POOL_QINDEX};
  // perFramebufDescriptorSet maps one DescriptorSet to each framebuf. There
  // must be a different DescriptorSet for each because the GPU might still be
  // using the previous DescriptorSet when rendering starts on the next frame.
  // cmdBuffers also maps one CommandBuffer to each framebuf. Hopefully this
  // pattern is becoming familiar to you.
  std::vector<std::shared_ptr<memory::DescriptorSet>> perFramebufDescriptorSet;
  // perFramebufUniform maps one Buffer to each framebuf just like
  // perFramebufDescriptorSet.
  std::vector<memory::Buffer> perFramebufUniform;
  // pipe0 generates the Pipeline (pipeline state object) for the RenderPass.
  science::PipeBuilder pipe0{pass};
  memory::Buffer vertexBuffer{cpool.vk.dev};
  memory::Buffer indexBuffer{cpool.vk.dev};

  // buildPass builds the vertex, index, and uniform buffers, descriptor sets,
  // and finally the RenderPass pass.
  int buildPass() {
    language::Device& dev = cpool.vk.dev;
    vertexBuffer.info.size = sizeof(vertices[0]) * vertices.size();
    vertexBuffer.info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    indexBuffer.info.size = sizeof(indices[0]) * indices.size();
    indexBuffer.info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (vertexBuffer.ctorAndBindDeviceLocal() ||
        indexBuffer.ctorAndBindDeviceLocal() ||
        stage.copy(vertexBuffer, 0 /*offset*/, vertices) ||
        stage.copy(indexBuffer, 0 /*offset*/, indices)) {
      return 1;
    }

    // Read compiled-in shaders from app into Vulkan.
    auto vert = std::make_shared<command::Shader>(dev);
    auto frag = std::make_shared<command::Shader>(dev);
    if (vert->loadSPV(spv_05indexbuffer_vert, sizeof(spv_05indexbuffer_vert)) ||
        frag->loadSPV(spv_05indexbuffer_frag, sizeof(spv_05indexbuffer_frag)) ||
        shaders.add(pipe0, vert) || shaders.add(pipe0, frag)) {
      return 1;
    }

    pipe0.info().dynamicStates.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
    pipe0.info().dynamicStates.emplace_back(VK_DYNAMIC_STATE_SCISSOR);
    return pipe0.addDepthImage({
               // These depth image formats will be tried in order:
               VK_FORMAT_D32_SFLOAT,
               VK_FORMAT_D32_SFLOAT_S8_UINT,
               VK_FORMAT_D24_UNORM_S8_UINT,
           }) ||
           pipe0.addVertexInput<st_05indexbuffer_vert>() ||
           shaders.finalizeDescriptorLibrary(descriptorLibrary) ||
           onResized(dev.swapChainInfo.imageExtent, memory::ASSUME_POOL_QINDEX);
  }

  int buildFramebuf(language::Framebuf& framebuf, size_t framebuf_i) {
    language::Device& dev = cpool.vk.dev;
    // Allocate the VkCommandBuffer handles. Only done for the first framebuf.
    if (framebuf_i == 0 &&
        cpool.reallocCmdBufs(cmdBuffers, dev.framebufs.size(), pass, 0)) {
      return 1;
    }

    // If this framebuffer has not had a uniform buffer set up yet, then:
    if (framebuf_i >= perFramebufUniform.size()) {
      // Add to the vector of uniform buffers.
      perFramebufUniform.emplace_back(cpool.vk.dev);
      auto& ubo = perFramebufUniform.back();
      ubo.info.size = sizeof(UniformBufferObject);
      ubo.info.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      if (ubo.ctorAndBindDeviceLocal()) {
        logE("perFramebufUniform[%zu].ctorError failed\n", framebuf_i);
        return 1;
      }
      VkDescriptorBufferInfo dsBuf;
      memset(&dsBuf, 0, sizeof(dsBuf));
      dsBuf.buffer = ubo.vk;
      dsBuf.range = ubo.info.size;
      std::shared_ptr<memory::DescriptorSet> ds = descriptorLibrary.makeSet(0);
      if (!ds) {
        logE("[%zu] descriptorLibrary.makeSet failed\n", framebuf_i);
        return 1;
      }
      perFramebufDescriptorSet.push_back(ds);
      if (ds->write(bindingIndexOfUniformBufferObject(), {dsBuf})) {
        logE("ds[%zu].write(perFramebufUniform[%zu]) failed\n", framebuf_i,
             framebuf_i);
        return 1;
      }
    }

    // Patch RenderPass pass with what has changed.
    auto& newSize = cpool.vk.dev.swapChainInfo.imageExtent;
    VkViewport& viewport = pipe0.info().viewports.at(0);
    viewport.width = (float)newSize.width;
    viewport.height = (float)newSize.height;

    pipe0.info().scissors.at(0).extent = newSize;

    VkBuffer vertBufs[] = {vertexBuffer.vk};
    VkDeviceSize offsets[] = {0};

    // Write drawing commands for each framebuf_i.
    auto& cmdBuffer = cmdBuffers.at(framebuf_i);
    if (cmdBuffer.beginSimultaneousUse() ||
        cmdBuffer.beginSubpass(pass, framebuf, 0) ||
        cmdBuffer.bindGraphicsPipelineAndDescriptors(
            *pipe0.pipe, 0, 1, &perFramebufDescriptorSet.at(framebuf_i)->vk) ||
        cmdBuffer.setViewport(0, 1, &viewport) ||
        cmdBuffer.setScissor(0, 1, &pipe0.info().scissors.at(0)) ||
        cmdBuffer.bindVertexBuffers(0, sizeof(vertBufs) / sizeof(vertBufs[0]),
                                    vertBufs, offsets) ||
        cmdBuffer.bindAndDraw(indices, indexBuffer.vk, 0 /*indexBufOffset*/) ||
        cmdBuffer.endRenderPass() || cmdBuffer.end()) {
      logE("buildFramebuf: command buffer [%zu] failed\n", framebuf_i);
      return 1;
    }
    return 0;
  }

  void buildUBO(UniformBufferObject& ubo, float time) {
    ubo.model = glm::rotate(glm::rotate(glm::mat4(1.0f), glm::radians(40.0f),
                                        glm::vec3(0.0f, 1, 0)),
                            time * glm::radians(90.0f), glm::vec3(0.0f, 0, 1));

    ubo.view = glm::lookAt(glm::vec3(2.0f, 0, 0),      // Object pose.
                           glm::vec3(0.0f, 0, -0.23),  // Camera pose.
                           glm::vec3(0.0f, 0, 1));     // Up vector.

    ubo.proj = glm::perspective(glm::radians(45.0f), cpool.vk.dev.aspectRatio(),
                                0.1f, 10.0f);

    // convert from OpenGL where clip coordinates +Y is up
    // to Vulkan where clip coordinates +Y is down. The other OpenGL/Vulkan
    // coordinate change is GLM_FORCE_DEPTH_ZERO_TO_ONE. For more information:
    // https://community.khronos.org/t/understand-vulkan-clipping/6940
    // https://matthewwellings.com/blog/the-new-vulkan-coordinate-system/
    // https://www.khronos.org/registry/vulkan/specs/1.0/html/vkspec.html#vertexpostproc-clipping
    ubo.proj[1][1] *= -1;
  }

  int paused{0};

  int windowShouldClose(GLFWwindow* window) {
    if (!glfwWindowShouldClose(window)) {
      glfwPollEvents();
      return GLFW_FALSE;
    }
#ifdef __ANDROID__
    // glfwIsPaused() is only for Android. It reports the app lifecycle.
    paused = glfwIsPaused() ? GLFW_TRUE : paused;
#endif
    if (!paused) {
      return GLFW_TRUE;
    }
    // This supports pause on all platforms. This is actually simpler than an
    // Android-specific path.
    logI("paused. Wait for unpause or exit.\n");
    glfwSetWindowShouldClose(window, GLFW_FALSE);
    while (paused) {
      glfwWaitEvents();
#ifdef __ANDROID__
      paused = glfwIsPaused() ? paused : GLFW_FALSE;
#endif
      if (glfwWindowShouldClose(window)) {
        logI("paused - got exit request.\n");
        return GLFW_TRUE;
      }
    }
    logI("unpaused.\n");
#ifdef __ANDROID__
    for (int retry = 0;; retry++) {
      if (glfwCreateWindowSurface(instance.vk, window, instance.pAllocator,
                                  &instance.surface)) {
        logE("unpause: glfwCreateWindowSurface failed\n");
        return GLFW_TRUE;
      }
      // Android's biggest difference is surface was destroyed when pause began
      // in onGLFWFocus() below. Recreate surface now. This is still faster than
      // exiting, then reloading the whole app.
      // glfwWindowShouldClose will cause your app to exit if it thinks your app
      // does not know to call glfwIsPaused().
      if (onResized(cpool.vk.dev.swapChainInfo.imageExtent,
                    memory::ASSUME_POOL_QINDEX)) {
        logE("unpause: create swapchain failed\n");
        return GLFW_TRUE;  // Ask app to exit.
      }
      if (instance.surface) {
        break;
      }
      logE("unpause: onResized destroyed surface, retry %d\n", retry);
      usleep(30000);
    }
#endif
    return GLFW_FALSE;
  }

  command::Semaphore imageAvailableSemaphore{cpool.vk.dev};
  command::Fence renderDoneFence{cpool.vk.dev};  // Only needed on __APPLE__
  unsigned frameCount{0};
  unsigned lastPrintedFrameCount{0};
  float animationTime{0};
  Timer elapsed;
  int redrawErrorCount{0};
  void redraw() {
    // if swapChain was reset (happens at any time on Android) stop.
    if (cpool.vk.dev.swapChain && redrawError()) {
      redrawErrorCount++;
    }
  }

  int redrawError() {
    if (elapsed.get() > 1.0) {
      logI("%d fps\n", frameCount - lastPrintedFrameCount);
      animationTime += elapsed.get();
      elapsed.reset();
      lastPrintedFrameCount = frameCount;
      if (animationTime > 4.0f) {
        animationTime -= 4.0f;
      }
    }

    language::Device& dev = cpool.vk.dev;
    uint32_t nextImage = 0;
    VkResult result = vkAcquireNextImageKHR(
        dev.dev, dev.swapChain, std::numeric_limits<uint64_t>::max(),
        imageAvailableSemaphore.vk, VK_NULL_HANDLE, &nextImage);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
      if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        if (
#ifdef __ANDROID__ /* surface being destroyed may return OUT_OF_DATE */
            dev.getSurface() &&
#endif
            onResized(dev.swapChainInfo.imageExtent,
                      memory::ASSUME_POOL_QINDEX)) {
          logE("vkAcquireNextImageKHR: OUT_OF_DATE, but onResized failed\n");
          return 1;
        }
        return 0;
      } else if (result == VK_ERROR_SURFACE_LOST_KHR) {
        // VK_ERROR_SURFACE_LOST_KHR can be recovered by rebuilding it.
        return 0;
      }
      logE("%s failed: %d (%s)\n", "vkAcquireNextImageKHR", result,
           string_VkResult(result));
      return 1;
    }

    // Generate commands to upload perFramebufUniform to submit later.
    std::shared_ptr<memory::Flight> flight;
    if (stage.mmap(perFramebufUniform.at(nextImage), 0 /*offset*/,
                   sizeof(UniformBufferObject), flight)) {
      logE("05indexbuffer: stage.mmap failed\n");
      return 1;
    }
    // flight->mmap() points to the uniform buffer. Write directly to it.
    buildUBO(*reinterpret_cast<UniformBufferObject*>(flight->mmap()),
             animationTime + elapsed.get());
    if (stage.flushButNotSubmit(flight)) {
      logE("05indexbuffer: stage.flushButNotSubmit failed\n");
      return 1;
    }

    command::SubmitInfo sub;
    sub.waitFor.emplace_back(imageAvailableSemaphore,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    sub.toSignal.emplace_back(renderSemaphore.vk);
    {
      command::CommandPool::lock_guard_t lock(cpool.lockmutex);
      if (cmdBuffers.at(nextImage).enqueue(lock, sub)) {
        logE("cmdBuffers[%u].enqueue failed\n", nextImage);
        return 1;
      }
      if (flight->canSubmit() &&
          (flight->end() || flight->enqueue(lock, sub))) {
        logE("flight[%u].enqueue failed\n", nextImage);
        return 1;
      }
      // Send all command buffers in sub over to the GPU with cpool.submit().
      // Since submit() is expensive, it is only done once per frame.
      if (cpool.submit(lock, stage.poolQindex, {sub}, renderDoneFence.vk)) {
        logE("submit[%u] failed\n", nextImage);
        return 1;
      }
    }

    if (dev.framebufs.at(nextImage).dirty) {
      logW("framebuf[%u] dirty and has not been rebuilt before present\n",
           nextImage);
    }
    VkSemaphore semaphores[] = {renderSemaphore.vk};
    VkSwapchainKHR swapChains[] = {dev.swapChain};

    VkPresentInfoKHR presentInfo;
    memset(&presentInfo, 0, sizeof(presentInfo));
    presentInfo.sType = autoSType(presentInfo);
    presentInfo.waitSemaphoreCount = sizeof(semaphores) / sizeof(semaphores[0]);
    presentInfo.pWaitSemaphores = semaphores;
    presentInfo.swapchainCount = sizeof(swapChains) / sizeof(swapChains[0]);
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &nextImage;

    result = vkQueuePresentKHR(presentQueue, &presentInfo);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
      if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        if (onResized(dev.swapChainInfo.imageExtent,
                      memory::ASSUME_POOL_QINDEX)) {
          logE("present: OUT_OF_DATE, but onResized failed\n");
          return 1;
        }
      } else if (result == VK_ERROR_SURFACE_LOST_KHR) {
        // VK_ERROR_SURFACE_LOST_KHR can be recovered by rebuilding it.
      } else {
        logE("%s failed: %d (%s)\n", "vkQueuePresentKHR", result,
             string_VkResult(result));
        return 1;
      }
      if (renderDoneFence.reset()) {
        logE("present incomplete, renderDoneFence.reset failed\n");
        return 1;
      }
      return 0;
    }
    // vkQueueWaitIdle() cleans up resource leaks from validation layers.
    if ((frameCount % 64) == 63) {
      result = vkQueueWaitIdle(presentQueue);
      if (result != VK_SUCCESS) {
        logE("%s failed: %d (%s)\n", "vkQueueWaitIdle", result,
             string_VkResult(result));
        return 1;
      }
    }
    result = renderDoneFence.waitMs(100);
    if (result != VK_SUCCESS) {
      logE("%s failed: %d (%s)\n", "renderDoneFence", result,
           string_VkResult(result));
      return 1;
    }
    if (renderDoneFence.reset()) {
      logE("renderDoneFence failed\n");
      return 1;
    }
    frameCount++;
    flight.reset();
    return 0;
  }

 public:
  int run(GLFWwindow* window) {
    if (cpool.ctorError() || imageAvailableSemaphore.ctorError() ||
        renderDoneFence.ctorError() || renderSemaphore.ctorError() ||
        buildPass()) {
      return 1;
    }

    glfwSetWindowUserPointer(window, this);
    glfwSetWindowSizeCallback(window, onGLFWResized);
    glfwSetWindowRefreshCallback(window, onGLFWRefresh);
    glfwSetWindowFocusCallback(window, onGLFWFocus);
    glfwSetKeyCallback(window, onGLFWkey);

    auto qfam_i = cpool.vk.dev.getQfamI(language::PRESENT);
    if (qfam_i == (decltype(qfam_i))(-1)) {
      logE("dev.getQfamI(%d) failed\n", language::PRESENT);
      return 1;
    }
    auto& qfam = cpool.vk.dev.qfams.at(qfam_i);
    if (qfam.queues.size() < 1) {
      logE("BUG: queue family PRESENT with %zu queues\n", qfam.queues.size());
      return 1;
    }
    presentQueue = qfam.queues.at(memory::ASSUME_PRESENT_QINDEX);

    // Begin main loop.
    while (!windowShouldClose(window)) {
      cpool.vk.dev.setFrameNumber(frameCount);
      redraw();
      if (redrawErrorCount > 0) {
        return 1;
      }
    }
    return cpool.deviceWaitIdle();
  }

  static void onGLFWResized(GLFWwindow* window, int w, int h) {
    auto self = reinterpret_cast<Example05*>(glfwGetWindowUserPointer(window));
    if (w == 0 || h == 0) {
      // Window was minimized or moved offscreen.
      return;
    }
    uint32_t width = w, height = h;
    if (self->onResized({width, height}, memory::ASSUME_POOL_QINDEX)) {
      logE("onGLFWResized: onResized failed!\n");
      exit(1);
    }
    self->redraw();
  }

  static void onGLFWRefresh(GLFWwindow* window) {
    static_cast<Example05*>(glfwGetWindowUserPointer(window))->redraw();
  }

  static void onGLFWFocus(GLFWwindow* window, int focused) {
#ifdef __ANDROID__
    auto self = reinterpret_cast<Example05*>(glfwGetWindowUserPointer(window));
    if (!focused) {
      // On Android, surface *must* *not* be used after this function returns.
      self->cpool.vk.dev.destroySurface();
    }
#else  /*__ANDROID__*/
    (void)window;
    (void)focused;
#endif /*__ANDROID__*/
  }

  static void onGLFWkey(GLFWwindow* window, int key, int /*scancode*/,
                        int action, int /*mods*/) {
    auto self = reinterpret_cast<Example05*>(glfwGetWindowUserPointer(window));
    if (action == GLFW_PRESS && key == GLFW_KEY_P) {
      // Toggle pause state.
      self->paused = self->paused ? GLFW_FALSE : GLFW_TRUE;
      // If now paused, use glfwSetWindowShouldClose to break the main loop.
      if (self->paused) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
      }
    }
  }
};

static int createApp(GLFWwindow* window) {
  int width, height, r = 1;  // Let GLFW-on-Android override the window size.
  glfwGetWindowSize(window, &width, &height);
  language::Instance inst;
  unsigned int eCount = 0;
  const char** e = glfwGetRequiredInstanceExtensions(&eCount);
  inst.requiredExtensions.insert(inst.requiredExtensions.end(), e, &e[eCount]);
  if (inst.ctorError(
          [](language::Instance& inst, void* window) -> VkResult {
            return glfwCreateWindowSurface(inst.vk, (GLFWwindow*)window,
                                           inst.pAllocator, &inst.surface);
          },
          window) ||
      // inst.open() takes a while, especially if validation layers are on.
      inst.open({(uint32_t)width, (uint32_t)height})) {
    logE("inst.ctorError or inst.open failed\n");
  } else if (!inst.devs.size()) {
    logE("No vulkan devices found (or driver missing?)\n");
  } else {
    r = std::make_shared<Example05>(inst)->run(window);
  }
  return r;  // Destroy inst only after app.
}

static int crossPlatformMain(int argc, char** argv) {
  (void)argc;
  (void)argv;
  if (!glfwInit()) {
    logE("glfwInit failed. Windowing system probably disabled.\n");
    return 1;
  }
  glfwSetErrorCallback([](int code, const char* msg) -> void {
    logE("glfw error %x: %s\n", code, msg);
  });
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* window = glfwCreateWindow(800, 600, "05indexbuffer Vulkan window",
                                        nullptr /*monitor for fullscreen*/,
                                        nullptr /*context object sharing*/);
  int r = createApp(window);
  glfwDestroyWindow(window);
  glfwTerminate();
  return r;
}

}  // namespace example

// OS-specific startup code.
#ifdef _WIN32
// Windows-specific startup.
int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  return example::crossPlatformMain(__argc, __argv);
}
#elif defined(__ANDROID__)
// Android-specific startup.
extern "C" void android_main(android_app* app) {
  glfwAndroidMain(app, example::crossPlatformMain);
}
#else
// Posix startup.
int main(int argc, char** argv) {
  return example::crossPlatformMain(argc, argv);
}
#endif
