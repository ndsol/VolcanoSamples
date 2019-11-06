/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This sample demonstrates how to build an app on Android. All the other
 * samples can be built and run on Android the same way this sample can be.
 */

#include "../src/base_application.h"

// Compile SPIR-V bytecode directly into application.
#include "04android/04android.frag.h"
#include "04android/04android.vert.h"

namespace example {

#include "04android/struct_04android.vert.h"

// Example04 is documented at github.com/ndsol/VolcanoSamples/04android/
class Example04 : public BaseApplication {
 public:
  Example04(language::Instance& instance) : BaseApplication{instance} {
    resizeFramebufListeners.emplace_back(std::make_pair(
        [](void* self, language::Framebuf& framebuf, size_t fbi,
           size_t) -> int {
          return static_cast<Example04*>(self)->buildFramebuf(framebuf, fbi);
        },
        this));
  }

 protected:
  command::Semaphore renderSemaphore{cpool.vk.dev};
  VkQueue presentQueue{VK_NULL_HANDLE};
  std::vector<command::CommandBuffer> cmdBuffers;
  // Use science::ShaderLibrary and science::PipeBuilder to make a Vertex Buffer
  science::ShaderLibrary shaders{cpool.vk.dev};
  science::PipeBuilder pipe0{pass};
  memory::Buffer vertexBuffer{cpool.vk.dev};
  // Use a memory::Stage to automate transferring data from host to GPU.
  memory::Stage stage{cpool, memory::ASSUME_POOL_QINDEX};

  // vertices is the host copy of the vertex buffer.
  std::vector<st_04android_vert> vertices;
  // scaleX, scaleY are for hi DPI screens that scale the UI.
  float scaleX{1.f}, scaleY{1.f};

  // maxPointers controls the size of the vertex buffer. Think of a tablet with
  // multi-touch: 10 fingers is a lot. 128 should be well beyond the limit.
  constexpr static int maxPointers = 128;

  // This sample draws lines (2 vertices per line).
  constexpr static int VERTS_PER_PRIMITIVE = 2;

  // maxVerticesSize() calculates the size of a vector of vertices.
  size_t maxVerticesSize() const { return maxPointers * VERTS_PER_PRIMITIVE; }

  // PointerState stores the location of each finger or mouse.
  struct PointerState {
    double x, y;  // Location of this pointer.
    unsigned b;   // Bit mask of buttons that are pressed.
  };

  // generateVertices fills the vertices vector.
  int generateVertices(const std::vector<PointerState>& states) {
    vertices.clear();
    float xMul = 2 * scaleX, yMul = 2 * scaleY;
#if defined(__linux__) && !defined(__ANDROID__)
    xMul = 2.f;  // Not all monitors report correctly on Linux.
    yMul = 2.f;
#endif
    switch (cpool.vk.dev.swapChainInfo.preTransform) {
      case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
      case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
      case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR:
      case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR:
        // Rotated dimensions
        yMul /= cpool.vk.dev.swapChainInfo.imageExtent.width;
        xMul /= cpool.vk.dev.swapChainInfo.imageExtent.height;
        break;
      default:
        // Normal dimensions
        xMul /= cpool.vk.dev.swapChainInfo.imageExtent.width;
        yMul /= cpool.vk.dev.swapChainInfo.imageExtent.height;
        break;
    }
    for (const auto& state : states) {
      float x = state.x * xMul - 1;
      float y = state.y * yMul - 1;
      vertices.push_back(st_04android_vert{{0, 0}, {1, 1, 1}});
      vertices.push_back(st_04android_vert{
          {x, y},
          {(state.b & 1) ? 1.f : 0.f, (state.b & 2) ? 1.f : 0.f,
           (state.b & 4) ? 1.f : 0.f}});
      if (vertices.size() >= maxVerticesSize()) {
        break;  // Just in case there are too many input pointers.
      }
    }

    size_t base = vertices.size();
    size_t total = 0;
    for (int jid = 0; jid <= GLFW_JOYSTICK_LAST; jid++) {
      if (glfwJoystickPresent(jid)) {
        int count;
        const float* axes = glfwGetJoystickAxes(jid, &count);
        if (axes) {
          total += count;
        }
      }
    }
    total = (total < 1) ? 1 : total;  // Prevent divide-by-zero.
    for (int jid = 0;
         vertices.size() < maxVerticesSize() && jid <= GLFW_JOYSTICK_LAST;
         jid++) {
      if (!glfwJoystickPresent(jid)) {
        continue;
      }

      int count;
      const float* axes = glfwGetJoystickAxes(jid, &count);
      if (!axes) {
        continue;
      }
      for (int aid = 0; vertices.size() < maxVerticesSize() && aid < count;
           aid++) {
        float y = (vertices.size() - base + 0.5f) / total - 1.;
        vertices.push_back(st_04android_vert{{0, y}, {1, 1, 1}});
        vertices.push_back(
            st_04android_vert{{axes[aid], y},
                              {(aid & 2) ? 1.f : 0.f, (aid & 4) ? 1.f : 0.f,
                               (aid & 8) ? 1.f : 0.f}});
      }
    }

    // Must always produce the max number of vertices. "Degenerate" vertices
    // are chosen to never be drawn. Otherwise the pipeline would have to be
    // rebuilt with a CommandBuffer specifying the number of vertices each time.
    // There is CommandBuffer:drawIndirect() to specify the number of vertices
    // in a buffer.
    while (vertices.size() < maxVerticesSize()) {
      vertices.push_back(st_04android_vert{{-2, -2}, {1, 0, 0}});
      vertices.push_back(st_04android_vert{{-2, -2}, {0, 1, 0}});
    }
    return 0;
  }

  int buildFramebuf(language::Framebuf& framebuf, size_t framebuf_i) {
    language::Device& dev = cpool.vk.dev;
    // Allocate the VkCommandBuffer handles. Only done for the first framebuf.
    if (framebuf_i == 0 &&
        cpool.reallocCmdBufs(cmdBuffers, dev.framebufs.size(), pass, 0)) {
      return 1;
    }

    // Patch pipe0 with what has changed.
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
        cmdBuffer.bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipe0.pipe) ||
        cmdBuffer.setViewport(0, 1, &viewport) ||
        cmdBuffer.setScissor(0, 1, &pipe0.info().scissors.at(0)) ||
        cmdBuffer.bindVertexBuffers(0, sizeof(vertBufs) / sizeof(vertBufs[0]),
                                    vertBufs, offsets) ||
        cmdBuffer.draw(maxVerticesSize(), 1, 0, 0) ||
        cmdBuffer.endRenderPass() || cmdBuffer.end()) {
      logE("buildFramebuf: command buffer [%zu] failed\n", framebuf_i);
      return 1;
    }
    return 0;
  }

  command::Semaphore imageAvailableSemaphore{cpool.vk.dev};
  // MoltenVK does not update the screen without a GPU-to-CPU fence.
  // renderDoneFence can be wrapped in #ifdef __APPLE__ with no ill effects.
  command::Fence renderDoneFence{cpool.vk.dev};
  unsigned frameCount{0};
  unsigned redrawErrorCount{0};

  void redraw() {
    // if swapChain was reset (happens at any time on Android) stop.
    if (cpool.vk.dev.swapChain && redrawError()) {
      redrawErrorCount++;
    }
  }

  int redrawError() {
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
    // Flight is a subclass of command::CommandBuffer. Generate commands
    // to upload vertices, but only enqueue it in SubmitInfo sub later.
    std::shared_ptr<memory::Flight> flight;
    size_t vSize = vertices.size() * sizeof(vertices[0]);
    if (stage.mmap(vertexBuffer, 0 /*offset*/, vSize, flight)) {
      logE("04android: stage.mmap failed\n");
      return 1;
    }
    // Copy from CPU vector 'vertices' to operation->mmap.
    memcpy(flight->mmap(), vertices.data(), vSize);

    if (stage.flushButNotSubmit(flight)) {
      logE("04android: stage.flushButNotSubmit failed\n");
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
      // Send all command buffers in sub over to the GPU with submit()
      // Since submit() is expensive, it is only done once per frame.
      if (cpool.submit(lock, stage.poolQindex, {sub}, renderDoneFence.vk)) {
        logE("submit failed\n");
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
    // Like in Sample 1, use cpool.vk.dev to pass the same dev to everything.
    language::Device& dev = cpool.vk.dev;

    vertexBuffer.info.size = sizeof(vertices[0]) * maxVerticesSize();
    vertexBuffer.info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    // Use a Science::PipeBuilder to simplify Vertex Buffer construction.
    pipe0.info().dynamicStates.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
    pipe0.info().dynamicStates.emplace_back(VK_DYNAMIC_STATE_SCISSOR);
    pipe0.info().asci.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    auto vert = std::make_shared<command::Shader>(dev);
    auto frag = std::make_shared<command::Shader>(dev);
    if (cpool.ctorError() || imageAvailableSemaphore.ctorError() ||
        renderDoneFence.ctorError() || renderSemaphore.ctorError() ||
        vertexBuffer.ctorAndBindDeviceLocal() ||
        pipe0.addVertexInput<st_04android_vert>() ||
        // Read compiled-in shaders from app into Vulkan.
        vert->loadSPV(spv_04android_vert, sizeof(spv_04android_vert)) ||
        frag->loadSPV(spv_04android_frag, sizeof(spv_04android_frag)) ||
        // Add shaders to ShaderLibrary, RenderPass, and pipe0.
        shaders.add(pipe0, vert) || shaders.add(pipe0, frag) ||
        // Note: DescriptorSet is not needed since all inputs to the shaders are
        // fixed-function. This sample just uses a vertex buffer:
        generateVertices(std::vector<PointerState>{}) ||
        stage.copy(vertexBuffer, 0 /*offset*/, vertices) ||
        // Manually resize language::Framebuf the first time.
        onResized(dev.swapChainInfo.imageExtent, memory::ASSUME_POOL_QINDEX)) {
      logE("04android run: ctorError failed\n");
      return 1;
    }

    glfwSetWindowUserPointer(window, this);
    glfwSetWindowSizeCallback(window, onGLFWResized);
    glfwSetWindowRefreshCallback(window, onGLFWrefresh);
    glfwSetWindowFocusCallback(window, onGLFWFocus);
    glfwGetWindowContentScale(window, &scaleX, &scaleY);
    glfwSetWindowContentScaleCallback(window, onGLFWcontentScale);
    glfwSetCursorEnterCallback(window, onGLFWcursorEnter);
#ifdef GLFW_HAS_MULTITOUCH
    glfwSetMultitouchEventCallback(window, onGLFWmultitouch);
#else  /*GLFW_HAS_MULTITOUCH*/
    glfwSetCursorPosCallback(window, onGLFWcursorPos);
    glfwSetMouseButtonCallback(window, onGLFWmouseButtons);
    // glfwSetScrollCallback(window, onGLFWscroll);  // Not used in this sample.
#endif /*GLFW_HAS_MULTITOUCH*/
    // glfwSetKeyCallback(window, handleGLFWkey);  // Not used in this sample.
    // glfwSetCharCallback(window, handleGLFWutf32Char);  // Not in this sample.

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
    presentQueue = qfam.queues.at(memory::ASSUME_PRESENT_QINDEX);

    // Begin main loop.
    unsigned lastPrintedFrameCount = 0;
    Timer elapsed;
    while (!glfwWindowShouldClose(window)) {
      glfwPollEvents();
      if (elapsed.get() > 1.0) {
        logI("%d fps\n", frameCount - lastPrintedFrameCount);
        elapsed.reset();
        lastPrintedFrameCount = frameCount;
      }
      dev.setFrameNumber(frameCount);
      redraw();
      if (redrawErrorCount > 0) {
        return 1;
      }
#ifndef __ANDROID__
      // Non-Android requires joystick polling.
      generateVertices(prevStates);
#endif /*_ANDROID__*/
    }
    return cpool.deviceWaitIdle();
  }

 protected:
  // prevStates is the previous states vector (from the last pointer event).
  std::vector<PointerState> prevStates;
  int prevMouseButtons{0};
  int prevMods{0};

  // onPointMove overrides BaseApplication and handles pointer events.
  void onPointMove(const std::vector<PointerState>& states, int leaveWindow) {
    if (leaveWindow) {
      // Draw nothing.
      generateVertices(std::vector<PointerState>{});
    } else {
      // Draw all pointers.
      generateVertices(states);
    }
    prevStates = states;
  }

  static void onGLFWResized(GLFWwindow* window, int w, int h) {
    auto self = reinterpret_cast<Example04*>(glfwGetWindowUserPointer(window));
    if (w == 0 || h == 0) {
      // Window was minimized or moved offscreen.
      return;
    }
    uint32_t width = w, height = h;
    if (self->onResized({width, height}, memory::ASSUME_POOL_QINDEX)) {
      logE("onGLFWResized: onResized failed!\n");
      exit(1);
    }
  }

  static void onGLFWrefresh(GLFWwindow* window) {
    static_cast<Example04*>(glfwGetWindowUserPointer(window))->redraw();
  }

  int enteredWindow{0};

  static void onGLFWFocus(GLFWwindow* window, int focused) {
    auto self = reinterpret_cast<Example04*>(glfwGetWindowUserPointer(window));
#ifdef __ANDROID__
    if (!focused) {
      // On Android, surface *must* *not* be used after this function returns.
      self->cpool.vk.dev.destroySurface();
    }
#else  /*__ANDROID__*/
    PointerState state;
    state.x = 0;
    state.y = 0;
    state.b = (unsigned)self->prevMouseButtons;
    glfwGetCursorPos(window, &state.x, &state.y);
    if (!focused) {  // Apply focus state to enter/leave state. GLFW does not.
      self->enteredWindow = false;
    } else {
      auto& imageExtent = self->cpool.vk.dev.swapChainInfo.imageExtent;
      self->enteredWindow = state.x >= 0 && state.y >= 0 &&
                            state.x < imageExtent.width &&
                            state.y < imageExtent.height;
    }
    self->onPointMove({state}, !self->enteredWindow);
#endif /*__ANDROID__*/
  }

  static void onGLFWcursorEnter(GLFWwindow* window, int entered) {
    auto self = reinterpret_cast<Example04*>(glfwGetWindowUserPointer(window));
    self->enteredWindow = entered;
#ifdef GLFW_HAS_MULTITOUCH
    std::vector<PointerState> states(self->prevStates);
#else  /*GLFW_HAS_MULTITOUCH*/
    PointerState state;
    state.x = 0;
    state.y = 0;
    state.b = (unsigned)self->prevMouseButtons;
    glfwGetCursorPos(window, &state.x, &state.y);
    std::vector<PointerState> states(state);
#endif /*GLFW_HAS_MULTITOUCH*/
    self->onPointMove(states, !entered);
  }

  static void onGLFWcontentScale(GLFWwindow* window, float x, float y) {
    auto self = reinterpret_cast<Example04*>(glfwGetWindowUserPointer(window));
    self->scaleX = x;
    self->scaleY = y;
  }

#ifdef GLFW_HAS_MULTITOUCH
  static void onGLFWmultitouch(GLFWwindow* window, GLFWinputEvent* events,
                               int eventCount, int /*mods*/) {
    std::vector<PointerState> states;
    auto self = reinterpret_cast<Example04*>(glfwGetWindowUserPointer(window));
    for (int i = 0; i < eventCount; i++) {
      if (events[i].inputDevice == GLFW_INPUT_JOYSTICK) {
        continue;
      }

      // Filter out events that say "FINGER just went to buttons == 0" because
      // leaving a stray line on the screen, pointing at where the vanished
      // finger last was? That looks awkward.
      if (events[i].inputDevice != GLFW_INPUT_FINGER || events[i].buttons) {
        states.push_back(
            PointerState{events[i].x, events[i].y, events[i].buttons});
      }
    }
    self->onPointMove(states, !self->enteredWindow);
  }
#else  /*GLFW_HAS_MULTITOUCH*/
  static void onGLFWcursorPos(GLFWwindow* window, double x, double y) {
    auto self = reinterpret_cast<Example04*>(glfwGetWindowUserPointer(window));
    PointerState state{.x = x, .y = y, .b = self->prevMouseButtons};
    self->onPointMove({state}, !self->enteredWindow);
  }

  static void onGLFWmouseButtons(GLFWwindow* window, int button, int pressed,
                                 int mods) {
    auto self = reinterpret_cast<Example04*>(glfwGetWindowUserPointer(window));
    self->prevMods = mods;
    if (pressed) {
      self->prevMouseButtons |= 1 << button;
    } else {
      self->prevMouseButtons &= ~(1 << button);
    }
  }
#endif /*GLFW_HAS_MULTITOUCH*/
};

static int createApp(GLFWwindow* window) {
  int width, height;  // Let GLFW-on-Android override the window size.
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
    return 1;
  }
  if (!inst.devs.size()) {
    logE("No vulkan devices found (or driver missing?)\n");
    return 1;
  }
  int r = std::make_shared<Example04>(inst)->run(window);
  // Here app is destroyed and any objects in app, before inst is destroyed.
  return r;  // Now inst is destroyed. This prevents errors.
}

static int crossPlatformMain(int argc, char** argv) {
  (void)argc;
  (void)argv;
  VkExtent2D startSize{800, 600};
  if (!glfwInit()) {
    logE("glfwInit failed. Windowing system probably disabled.\n");
    return 1;
  }
  glfwSetErrorCallback([](int code, const char* msg) -> void {
    logE("glfw error %x: %s\n", code, msg);
  });

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* window = glfwCreateWindow(
      startSize.width, startSize.height, "04android Vulkan window",
      nullptr /*monitor for fullscreen*/, nullptr /*context object sharing*/);
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
