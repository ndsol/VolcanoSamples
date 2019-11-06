/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This sample demonstrates a minimal app that renders a triangle.
 */

// base_application.h pulls in GLFW
#include <fcntl.h> /* for _O_TEXT */
#ifdef _WIN32
#include <io.h> /* for _open_osfhandle */
#endif

#include <iostream>

#include "../src/base_application.h"

// Compile SPIR-V bytecode directly into application.
#include "01glfw/01glfw.frag.h"
#include "01glfw/01glfw.vert.h"

static void handleGlfwErrors(int error, const char* description) {
  printf("glfw error %d: %s\n", error, description);
}

// Wrap the function glfwCreateWindowSurface for Instance::ctorError():
static VkResult handleCreateSurface(language::Instance& inst, void* window) {
  return glfwCreateWindowSurface(inst.vk, (GLFWwindow*)window, nullptr,
                                 &inst.surface);
}

const int WIDTH = 800, HEIGHT = 600;

// Example01 is documented at github.com/ndsol/VolcanoSamples/01glfw/
class Example01 : public BaseApplication {
 public:
  command::Semaphore renderSemaphore{cpool.vk.dev};
  // pipe0 is the Vulkan Pipeline.
  std::shared_ptr<command::Pipeline> pipe0;
  // cmdBuffers are a vector because there is one per framebuffer.
  std::vector<command::CommandBuffer> cmdBuffers;

  Example01(language::Instance& instance)
      : BaseApplication{instance},
        // Ask CommandPoolContainer's RenderPass for a new pipeline object.
        pipe0{pass.addPipeline()} {
    // Register a callback with CommandPoolContainer::resizeFramebufListeners.
    resizeFramebufListeners.emplace_back(std::make_pair(
        [](void* self, language::Framebuf& framebuf, size_t fbi,
           size_t /*poolQindex*/) -> int {
          return static_cast<Example01*>(self)->buildFramebuf(framebuf, fbi);
        },
        this));
  }

  int buildFramebuf(language::Framebuf& framebuf, size_t framebuf_i) {
    language::Device& dev = cpool.vk.dev;
    // Allocate the VkCommandBuffer handles. Only done for the first framebuf.
    if (framebuf_i == 0 &&
        cpool.reallocCmdBufs(cmdBuffers, dev.framebufs.size(), pass, 0)) {
      return 1;
    }

    // Patch viewport.
    auto& newSize = dev.swapChainInfo.imageExtent;
    VkViewport& viewport = pipe0->info.viewports.at(0);
    viewport.width = (float)newSize.width;
    viewport.height = (float)newSize.height;

    // Patch scissors.
    pipe0->info.scissors.at(0).extent = newSize;

    // Write drawing commands for each framebuf_i.
    auto& cmdBuffer = cmdBuffers.at(framebuf_i);
    if (cmdBuffer.beginSimultaneousUse() ||
        // Begin RenderPass. There is only one subpass, i.e. subpass 0.
        cmdBuffer.beginSubpass(pass, framebuf, 0) ||
        cmdBuffer.bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe0) ||
        cmdBuffer.setViewport(0, 1, &viewport) ||
        cmdBuffer.setScissor(0, 1, &pipe0->info.scissors.at(0)) ||
        cmdBuffer.draw(3, 1, 0, 0) ||
        // End RenderPass.
        cmdBuffer.endRenderPass() || cmdBuffer.end()) {
      printf("buildFramebuf: cmdBuffer [%zu] failed\n", framebuf_i);
      return 1;
    }
    return 0;
  }

  int run(GLFWwindow* window) {
    language::Device& dev = cpool.vk.dev;
    pipe0->info.dynamicStates.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
    pipe0->info.dynamicStates.emplace_back(VK_DYNAMIC_STATE_SCISSOR);

    command::Semaphore imageAvailableSemaphore(dev);
    auto vshader = std::make_shared<command::Shader>(dev);
    auto fshader = std::make_shared<command::Shader>(dev);

    if (cpool.ctorError() || imageAvailableSemaphore.ctorError() ||
        renderSemaphore.ctorError() ||
        // Load the SPIR-V bytecode from the headers into Vulkan.
        vshader->loadSPV(spv_01glfw_vert, sizeof(spv_01glfw_vert)) ||
        fshader->loadSPV(spv_01glfw_frag, sizeof(spv_01glfw_frag)) ||
        // Add the shaders to pipeline.
        pipe0->info.addShader(pass, vshader, VK_SHADER_STAGE_VERTEX_BIT) ||
        pipe0->info.addShader(pass, fshader, VK_SHADER_STAGE_FRAGMENT_BIT)) {
      return 1;
    }

    glfwSetWindowAttrib(window, GLFW_RESIZABLE, false);
    auto& ex = dev.swapChainInfo.imageExtent;

    // CommandPoolContainer::onResized calls buildFramebuf for each element of
    // cmdBuffers. This is not fully ready for resizing yet, but is close.
    if (onResized(ex, memory::ASSUME_POOL_QINDEX)) {
      return 1;
    }

    auto qfam_i = dev.getQfamI(language::PRESENT);
    if (qfam_i == (decltype(qfam_i))(-1)) {
      logE("dev.getQfamI(%d) failed\n", language::PRESENT);
      return 1;
    }
    auto& qfam = dev.qfams.at(qfam_i);
    if (qfam.queues.size() < 1) {
      logE("BUG: queue family PRESENT with %zu queues\n", qfam.queues.size());
      return 1;
    }
    VkQueue presentQueue = qfam.queues.at(memory::ASSUME_PRESENT_QINDEX);

    // Begin main loop.
    unsigned frameCount = 0, lastPrintedFrameCount = 0;
    Timer elapsed;
    while (!glfwWindowShouldClose(window)) {
      glfwPollEvents();
      frameCount++;
      if (elapsed.get() > 1.0) {
        printf("%d fps\n", frameCount - lastPrintedFrameCount);
        elapsed.reset();
        lastPrintedFrameCount = frameCount;
      }
      dev.setFrameNumber(frameCount);

      if (!dev.swapChain) {
        // if swapChain was reset (happens at any time on Android) go back.
        continue;
      }
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
              onResized(ex, memory::ASSUME_POOL_QINDEX)) {
            logE("vkAcquireNextImageKHR: OUT_OF_DATE, but onResized failed\n");
            return 1;
          }
        } else if (result == VK_ERROR_SURFACE_LOST_KHR) {
          // VK_ERROR_SURFACE_LOST_KHR can be recovered by rebuilding it.
        } else {
          logE("%s failed: %d (%s)\n", "vkAcquireNextImageKHR", result,
               string_VkResult(result));
          return 1;
        }
        continue;
      }

      command::SubmitInfo info;
      info.waitFor.emplace_back(imageAvailableSemaphore,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
      info.toSignal.emplace_back(renderSemaphore.vk);

      {
        command::CommandPool::lock_guard_t lock(cpool.lockmutex);
        if (cmdBuffers.at(nextImage).enqueue(lock, info) ||
            cpool.submit(lock, memory::ASSUME_POOL_QINDEX, {info})) {
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
      presentInfo.waitSemaphoreCount =
          sizeof(semaphores) / sizeof(semaphores[0]);
      presentInfo.pWaitSemaphores = semaphores;
      presentInfo.swapchainCount = sizeof(swapChains) / sizeof(swapChains[0]);
      presentInfo.pSwapchains = swapChains;
      presentInfo.pImageIndices = &nextImage;

      result = vkQueuePresentKHR(presentQueue, &presentInfo);
      if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
          if (onResized(ex, memory::ASSUME_POOL_QINDEX)) {
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
        continue;
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
    }
    return cpool.deviceWaitIdle();
  }
};

static int createApp(GLFWwindow* window) {
  int width, height;  // Let GLFW-on-Android override the window size.
  glfwGetWindowSize(window, &width, &height);

  language::Instance inst;
  unsigned int eCount = 0;
  const char** e = glfwGetRequiredInstanceExtensions(&eCount);
  inst.requiredExtensions.insert(inst.requiredExtensions.end(), e, &e[eCount]);
  if (inst.ctorError(handleCreateSurface, window) ||
      // inst.open() takes a while, especially if validation layers are on.
      inst.open({(uint32_t)width, (uint32_t)height})) {
    printf("inst.ctorError or inst.open failed\n");
    return 1;
  }
  if (!inst.devs.size()) {
    printf("No vulkan devices found (or driver missing?)\n");
    return 1;
  }
  int r = std::make_shared<Example01>(inst)->run(window);
  // Here app is destroyed and any objects in app, before inst is destroyed.
  return r;  // Now inst is destroyed. This prevents errors.
}

static int crossPlatformMain(int argc, char** argv) {
  (void)argc;  // Not used. Silence compiler warning.
  (void)argv;  // Not used. Silence compiler warning.

#ifdef _WIN32
  // See https://stackoverflow.com/questions/191842
  AllocConsole();
  DWORD n[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  HANDLE h[sizeof(n) / sizeof(n[0])];
  FILE* f[sizeof(n) / sizeof(n[0])];
  for (int i = 0; i < sizeof(n) / sizeof(n[0]); i++) {
    h[i] = GetStdHandle(n[i]);
    if (h[i] == INVALID_HANDLE_VALUE) {
      MessageBox(NULL, "STD_*_HANDLE", "Error", MB_OK);
      return 1;
    }
    int fd = _open_osfhandle((intptr_t)h[i], _O_TEXT);
    if (fd < 0) {
      MessageBox(NULL, "_open_osfhandle()", "Error", MB_OK);
      return 1;
    }
    setvbuf((f[i] = _fdopen(fd, "r")), NULL, _IONBF, 0);
  }
  std::ios_base::sync_with_stdio();
  freopen_s(&f[0], "CONIN$", "r", stdin);
  freopen_s(&f[1], "CONOUT$", "w", stdout);
  freopen_s(&f[2], "CONOUT$", "w", stderr);
  std::wcout.clear();  // Visual Studio 2007+ workaround:
  std::cout.clear();   // C++ standard streams get set to error state
  std::wcerr.clear();  // during startup. Clear them all.
  std::cerr.clear();
  std::wcin.clear();
  std::cin.clear();
#endif
  glfwSetErrorCallback(handleGlfwErrors);
  glfwInit();

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

  GLFWwindow* window = glfwCreateWindow(WIDTH, HEIGHT, "My Title",
                                        nullptr /*monitor for fullscreen*/,
                                        nullptr /*context object sharing*/);
  if (!window) {
    printf("glfwCreateWindow failed\n");
    return 1;
  }

  int r = createApp(window);
  glfwDestroyWindow(window);
  glfwTerminate();
  return r;
}

// OS-specific startup code.
#ifdef _WIN32
// Windows-specific startup.
int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  return crossPlatformMain(__argc, __argv);
}
#elif defined(__ANDROID__)
// Android-specific startup.
extern "C" void android_main(android_app* app) {
  glfwAndroidMain(app, crossPlatformMain);
}
#else
// Posix startup.
int main(int argc, char** argv) { return crossPlatformMain(argc, argv); }
#endif
