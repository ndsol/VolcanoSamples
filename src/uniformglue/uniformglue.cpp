/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * Implments the acquire/present logic for class UniformGlue.
 */

#include "uniformglue.h"

#include <limits>

#include "imgui.h"

using namespace std::placeholders;

UniformGlue::UniformGlue(BaseApplication& app, GLFWwindow* window,
                         size_t maxLayoutIndex, unsigned uboBindingIndex,
                         size_t uboSize)
    : app(app),
      maxLayoutIndex(maxLayoutIndex),
      imGuiLayoutIndex(maxLayoutIndex + 1),
      imGuiDSetIndex(0),
      uboBindingIndex(uboBindingIndex),
      uboSize(uboSize),
      window(window) {
  insertedResizeFn = std::make_pair(
      [](void* self, language::Framebuf&, size_t fbi, size_t qi) -> int {
        return static_cast<UniformGlue*>(self)->buildFramebuf(fbi, qi);
      },
      this);
  app.resizeFramebufListeners.emplace_back(insertedResizeFn);
  fonts.push_back(std::make_shared<ImFontConfig>());
}

UniformGlue::~UniformGlue() {
  if (imGuiBufMmap) {
    imGuiBuf.mem.munmap();
    imGuiBufMmap = nullptr;
  }

  auto& rl = app.resizeFramebufListeners;
  rl.erase(std::remove(rl.begin(), rl.end(), insertedResizeFn), rl.end());
}

int UniformGlue::initVertexBuffer(const void* vertices, size_t verticesSize) {
  if (!moveVertexBufReason.empty()) {
    logE("UniformGlue::initPipeBuilderFrom failed: %s\n",
         moveVertexBufReason.c_str());
    return 1;
  }
  if (!verticesSize) {
    // If verticesSize == 0, there is no way to tell if called a second time.
    logE("FIXME: UniformGlue::vertices must have at least one element\n");
    return 1;
  }

  // Check if being called a second time:
  if (vertexBuffer.info.size) {
    // initUniform has already set up vertexBuffer. Do nothing.
    return 0;
  }

  if (imageAvailableSemaphore.ctorError() || renderDoneFence.ctorError() ||
      renderSemaphore.ctorError()) {
    logE("UniformGlue::initPipeBuilderFrom: semaphore or fence failed\n");
    return 1;
  }

  return updateVertexIndexBuffer(vertices, verticesSize);
}

int UniformGlue::updateVertexIndexBuffer(const void* vertices,
                                         size_t verticesSize) {
  if (!moveVertexBufReason.empty()) {
    logE("UniformGlue::updateVertexIndexBuffer failed: %s\n",
         moveVertexBufReason.c_str());
    return 1;
  }

  vertexBuffer.info.size = verticesSize;
  vertexBuffer.info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  indexBuffer.info.size = sizeof(indices[0]) * indices.size();
  indexBuffer.info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

  std::shared_ptr<memory::Flight> flight;
  if (vertexBuffer.ctorAndBindDeviceLocal() ||
      indexBuffer.ctorAndBindDeviceLocal() ||
      stage.mmap(vertexBuffer, 0 /*offset*/, verticesSize, flight)) {
    logE("UniformGlue::updateVertexBuffer: vertex or index buffer failed\n");
    return 1;
  }
  memcpy(flight->mmap(), vertices, verticesSize);
  if (stage.flushAndWait(flight) ||
      stage.copy(indexBuffer, 0 /*offset*/, indices)) {
    logE("UniformGlue::updateVertexBuffer: stage.copy failed\n");
    return 1;
  }
  return 0;
}

void UniformGlue::abortFrame() {
  nextImage = (uint32_t)-1;
  stillHaveAcquiredImage = true;
}

int UniformGlue::acquire() {
  auto& dev = app.cpool.vk.dev;
  if (needRebuild) {
    if (app.onResized(dev.swapChainInfo.imageExtent,
                      memory::ASSUME_POOL_QINDEX)) {
      logE("UniformGlue::acquire: onResized failed\n");
      return 1;
    }
    needRebuild = false;
    stillHaveAcquiredImage = false;
  }
  // if swapChain was reset (happens at any time on Android) stop.
  if (!dev.swapChain) {
    abortFrame();
    stillHaveAcquiredImage = false;
    return 0;
  }
  if (stillHaveAcquiredImage) {
    nextImage = lastAcquiredImage;
    return 0;
  }
  dev.setFrameNumber(frameNumber);
  nextImage = 0;
  VkResult result = vkAcquireNextImageKHR(
      dev.dev, dev.swapChain, std::numeric_limits<uint64_t>::max(),
      imageAvailableSemaphore.vk, VK_NULL_HANDLE, &nextImage);
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
      if (
#ifdef __ANDROID__ /* surface being destroyed may return OUT_OF_DATE */
          dev.getSurface() &&
#endif
          app.onResized(dev.swapChainInfo.imageExtent,
                        memory::ASSUME_POOL_QINDEX)) {
        logE("vkAcquireNextImageKHR: OUT_OF_DATE, but onResized failed\n");
        return 1;
      }
      abortFrame();
      return 0;
    } else if (result == VK_ERROR_SURFACE_LOST_KHR) {
      // VK_ERROR_SURFACE_LOST_KHR can be recovered by rebuilding it.
      abortFrame();
      return 0;
    }
    logE("%s failed: %d (%s)\n", "vkAcquireNextImageKHR", result,
         string_VkResult(result));
    return 1;
  }
  lastAcquiredImage = nextImage;
  return 0;
}

int UniformGlue::submit(std::shared_ptr<memory::Flight>& flight,
                        command::SubmitInfo& sub) {
  if (isAborted()) {
    // flight->reset() aborts the in-progress transfer.
    if (flight->canSubmit() && (flight->end() || flight->reset())) {
      logE("flight end or reset failed\n");
      return 1;
    }
    // flight.reset() returns the operation to the stage for re-use.
    flight.reset();
    return 0;
  }

  if (isImguiAvailable()) {
    // ImGui::Render populates data which is then sent to imGuiRender.
    ImGui::Render();
    if (checkImGuiBufSize(ImGui::GetDrawData())) {
      if (imGuiBuf.reset()) {
        logE("UniformGlue::submit: imGuiBuf.reset failed");
        return 1;
      }
      // Communicate to main loop it needs to restart with new cmdBuffers.
      // Rebuild command buffers because imGuiBuf was destroyed / reallocated.
      needRebuild = true;
      abortFrame();
    } else {
      if (imGuiRender(ImGui::GetDrawData())) {
        logE("UniformGlue::submit[%u]: imGuiRender failed\n", nextImage);
        return 1;
      }
    }
  }
  if (flight && stage.flushButNotSubmit(flight)) {
    logE("UniformGlue::submit: stage.flushButNotSubmit failed\n");
    return 1;
  }

  if (stillHaveAcquiredImage) {
    stillHaveAcquiredImage = false;
    lastAcquiredImage = (uint32_t)-1;
  }
  sub.waitFor.emplace_back(imageAvailableSemaphore,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  sub.toSignal.emplace_back(renderSemaphore.vk);

  {
    command::CommandPool::lock_guard_t lock(app.cpool.lockmutex);
    if (flight && flight->canSubmit() &&
        (
            // end() and enqueue() only if flight exists and canSubmit() is
            // true. If flight->end() fails, do not call enqueue().
            flight->end() || flight->enqueue(lock, sub))) {
      logE("UniformGlue::submit: end or enqueue failed\n");
      return 1;
    }
    if (cmdBuffers.at(nextImage).enqueue(lock, sub) ||
        app.cpool.submit(lock, stage.poolQindex, {sub}, renderDoneFence.vk)) {
      logE("UniformGlue::submit: app.cpool.submit failed\n");
      return 1;
    }
  }

  if (app.cpool.vk.dev.framebufs.at(nextImage).dirty) {
    logW("framebuf[%u] dirty and has not been rebuilt before present\n",
         nextImage);
  }
  VkSemaphore semaphores[] = {renderSemaphore.vk};
  VkSwapchainKHR swapChains[] = {app.cpool.vk.dev.swapChain};

  VkPresentInfoKHR presentInfo;
  memset(&presentInfo, 0, sizeof(presentInfo));
  presentInfo.sType = autoSType(presentInfo);
  presentInfo.waitSemaphoreCount = sizeof(semaphores) / sizeof(semaphores[0]);
  presentInfo.pWaitSemaphores = semaphores;
  presentInfo.swapchainCount = sizeof(swapChains) / sizeof(swapChains[0]);
  presentInfo.pSwapchains = swapChains;
  presentInfo.pImageIndices = &nextImage;

  VkResult result = vkQueuePresentKHR(presentQueue, &presentInfo);
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
      if (app.onResized(app.cpool.vk.dev.swapChainInfo.imageExtent,
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
  if ((frameNumber % 64) == 63) {
    result = vkQueueWaitIdle(presentQueue);
    if (result != VK_SUCCESS) {
      logE("%s failed: %d (%s)\n", "vkQueueWaitIdle", result,
           string_VkResult(result));
      return 1;
    }
  }
  for (uint32_t count = 0;;) {
    VkResult v = renderDoneFence.waitMs(100);
    if (v == VK_SUCCESS) {
      break;
    }
    if (v == VK_TIMEOUT && count < 10) {
      // Warn but continue to wait
      count++;
      logW("%s after %ums still waiting\n", "renderDoneFence", count * 100);
      continue;
    }
    logE("%s failed: %d (%s)\n", "renderDoneFence", v, string_VkResult(v));
    return 1;
  }
  if (renderDoneFence.reset()) {
    logE("UniformGlue::submit: renderDoneFence.reset failed\n");
    return 1;
  }
  flight.reset();
  frameNumber++;
  return 0;
}

int UniformGlue::windowShouldClose() {
  if (!glfwWindowShouldClose(window)) {
    if (fastButtons) {
      if (isImguiAvailable()) {
        auto& io = ImGui::GetIO();
        for (size_t i = 0; i < sizeof(io.MouseDown) / sizeof(io.MouseDown[0]);
             i++) {
          if (fastButtons & (1u << i)) {
            io.MouseDown[i] = 0;
          }
        }
      }
      fastButtons = 0;
    }
    curFrameButtons = 0;
    glfwPollEvents();

    // Update ImGui
    float currentTimestamp = Timer::now();
    if (isImguiAvailable()) {
      auto& io = ImGui::GetIO();
      io.MouseWheel = imguiScrollY;
      // The next frame should be starting right away, compute delta here.
      io.DeltaTime = currentTimestamp - imGuiTimestamp;
    }
    imguiScrollY = 0.0f;
    imGuiTimestamp = currentTimestamp;
    if (!paused) {
      return GLFW_FALSE;
    }
  }
  glfwSetWindowShouldClose(window, GLFW_FALSE);

#ifdef __ANDROID__
  // glfwIsPaused() is only for Android. It reports the app lifecycle.
  paused = glfwIsPaused() ? GLFW_TRUE : paused;
#endif
  // This supports pause on all platforms. This is actually simpler than an
  // Android-specific path.
  if (!paused) {
    return GLFW_TRUE;
  }
  while (paused) {
    glfwWaitEvents();
#ifdef __ANDROID__
    paused = glfwIsPaused() ? paused : false;
#endif
    if (glfwWindowShouldClose(window)) {
      return GLFW_TRUE;
    }
  }

#ifdef __ANDROID__
  for (int retry = 0;; retry++) {
    if (glfwCreateWindowSurface(app.instance.vk, window,
                                app.instance.pAllocator,
                                &app.instance.surface)) {
      logE("unpause: glfwCreateWindowSurface failed\n");
      return GLFW_TRUE;  // Ask app to exit.
    }
    // Android's biggest difference is surface was destroyed when pause began
    // in onGLFWFocus() below. Recreate surface now. This is still faster than
    // exiting, then reloading the whole app.
    // glfwWindowShouldClose will cause your app to exit if it thinks your app
    // does not know to call glfwIsPaused().
    if (app.onResized(app.cpool.vk.dev.swapChainInfo.imageExtent,
                      memory::ASSUME_POOL_QINDEX)) {
      logE("unpause: create swapchain failed\n");
      return GLFW_TRUE;  // Ask app to exit.
    }
    if (app.instance.surface) {
      break;
    }
    if (retry > 2) {
      logE("unpause: onResized destroyed surface %d times, exiting\n", retry);
      glfwSetWindowShouldClose(window, GLFW_TRUE);
      return GLFW_TRUE;
    }
    logE("unpause: onResized destroyed surface, retry %d\n", retry);
    usleep(30000);
  }
#endif
  return GLFW_FALSE;
}

int UniformGlue::buildFramebuf(size_t framebuf_i, size_t poolQindex) {
  if (framebuf_i == 0) {
    size_t want = app.cpool.vk.dev.framebufs.size();
#ifdef __ANDROID__ /*VK_PRESENT_MODE_MAILBOX_KHR uses 4 framebuffers*/
    want = want < 4 ? 4 : want;  // May need that many, ok if not used.
#endif                           /*__ANDROID__*/
    if (cmdBuffers.size() < want &&
        app.cpool.reallocCmdBufs(cmdBuffers, want, app.pass, 0, poolQindex)) {
      return 1;
    }
  }

  if (uniform.size() > framebuf_i) {
    // This framebuffer already has a uniform buffer that was set up for it.
    return 0;
  }
  // Add to the vector of uniform buffers.
  uniform.emplace_back(app.cpool.vk.dev);
  auto& ubo = uniform.back();
  ubo.info.size = uboSize;
  ubo.info.usage |= uniformUsageBits;
  if (ubo.ctorAndBindDeviceLocal()) {
    logE("uniform[%zu].ctorError failed\n", framebuf_i);
    return 1;
  }

  std::shared_ptr<memory::DescriptorSet> ds =
      descriptorLibrary.makeSet(0, maxLayoutIndex);
  if (!ds) {
    logE("[%zu] descriptorLibrary.makeSet failed for uniform buffer\n",
         framebuf_i);
    return 1;
  }
  descriptorSet.push_back(ds);

  VkDescriptorBufferInfo dsBuf;
  memset(&dsBuf, 0, sizeof(dsBuf));
  dsBuf.buffer = ubo.vk;
  dsBuf.range = ubo.info.size;

  // Limit the available range if requested by perFrameUboSize
  if (perFrameUboSize != 0 && dsBuf.range > (VkDeviceSize)perFrameUboSize) {
    dsBuf.range = (VkDeviceSize)perFrameUboSize;
  }

  auto& limits = shaders.dev.physProp.properties.limits;
  if (dsBuf.range > (VkDeviceSize)limits.maxUniformBufferRange) {
    logW("buildFramebuf(%zu): uniform size %zu vs maxUniformBufferRange %zu\n",
         framebuf_i, (size_t)ubo.info.size,
         (size_t)limits.maxUniformBufferRange);
    dsBuf.range = (VkDeviceSize)limits.maxUniformBufferRange;
  }

  char name[256];
  snprintf(name, sizeof(name), "uglue.descriptorSet[%zu]",
           descriptorSet.size() - 1);
  if (ds->setName(name) || ds->write(uboBindingIndex, {dsBuf})) {
    logE("ds[%zu].write(uniform[%zu]) failed\n", framebuf_i, framebuf_i);
    return 1;
  }
  return 0;
}
