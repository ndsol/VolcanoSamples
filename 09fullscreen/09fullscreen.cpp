/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This sample demonstrates how to switch in and out of fullscreen mode.
 */

#ifdef __ANDROID__
#include <android/window.h>          /* for AWINDOW_FLAG_KEEP_SCREEN_ON */
#include <android_native_app_glue.h> /* for struct android_app */
#endif
#include <src/science/science.h>

#include "../src/uniformglue/uniformglue.h"

// uniform_glue.h has already #included some glm headers.
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "09fullscreen/09fullscreen.frag.h"
#include "09fullscreen/09fullscreen.vert.h"
#include "imgui.h"

namespace example {

#include "09fullscreen/struct_09fullscreen.vert.h"

const std::vector<st_09fullscreen_vert> vertices = {
    {{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}},
    {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}},

    {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},
    {{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}}};

const std::vector<uint32_t> indices = {
    0, 1, 2,  // Triangle 1 uses vertices[0] - vertices[2]
    2, 3, 0,  // Triangle 2 uses vertices[0] - vertices[3]
    4, 5, 6,  // Triangle 3 uses vertices[4] - vertices[6]
    6, 7, 4,  // Triangle 4 uses vertices[4] - vertices[7]
};

const int INIT_WIDTH = 800, INIT_HEIGHT = 600;

// Example09 is documented at github.com/ndsol/VolcanoSamples/09fullscreen/
class Example09 : public BaseApplication {
 public:
  static constexpr size_t sampleLayout = 0;
  Example09(language::Instance& instance, GLFWwindow* window)
      : BaseApplication{instance},
        uglue{*this, window, sampleLayout /*maxLayoutIndex*/,
              bindingIndexOfUniformBufferObject(),
              sizeof(UniformBufferObject)} {
    resizeFramebufListeners.emplace_back(std::make_pair(
        [](void* self, language::Framebuf& framebuf, size_t fbi,
           size_t) -> int {
          return static_cast<Example09*>(self)->buildFramebuf(framebuf, fbi);
        },
        this));
    uglue.redrawListeners.emplace_back(std::make_pair(
        [](void* self, std::shared_ptr<memory::Flight>& flight) -> int {
          return static_cast<Example09*>(self)->redraw(flight);
        },
        this));
#ifdef VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME
    hasExtExclusive = cpool.vk.dev.isExtensionAvailable(
        VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
#endif /*VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME*/
  }

 protected:
  UniformGlue uglue;
  UniformBufferObject ubo;
  science::PipeBuilder pipe0{pass};
  GLFWfullscreen fs;

  // GLFW window transparency/composited (if the user requests it)
  // Android uses AndroidManifest.xml (android:theme) - see README.md
  bool userTransparentRequest{false};
#ifdef __APPLE__
  // macOS 10.7 "spaces" maximized window
  bool is10_7_max{false};
#endif /*__APPLE__*/
#ifdef VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME
  // win32 "full screen exclusive"
  bool hasExtExclusive;
  VkFullScreenExclusiveEXT curExclusiveMode{
      VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT};
#endif /*VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME*/
  // wantNormal is if the user wants to switch to windowed mode.
  bool wantNormal{false};
  // wantMax is if the user wants to switch to maximized mode.
  int wantMax{0};
  // wantMonitor is if the user wants fullscreen (or to switch monitors).
  GLFWmonitor* wantMonitor{nullptr};

  int buildPass() {
    // Copy indices.
    uglue.indices = indices;
    auto vert = std::make_shared<command::Shader>(cpool.vk.dev);
    auto frag = std::make_shared<command::Shader>(cpool.vk.dev);
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
           vert->loadSPV(spv_09fullscreen_vert,
                         sizeof(spv_09fullscreen_vert)) ||
           frag->loadSPV(spv_09fullscreen_frag,
                         sizeof(spv_09fullscreen_frag)) ||
           uglue.shaders.add(pipe0, vert) || uglue.shaders.add(pipe0, frag) ||
           uglue.initPipeBuilderFrom(pipe0, vertices) ||
           uglue.buildPassAndTriggerResize();
  }

  int buildFramebuf(language::Framebuf& framebuf, size_t framebuf_i) {
    char name[256];
    snprintf(name, sizeof(name), "uglue.descriptorSet[%zu]", framebuf_i);
    if (uglue.descriptorSet.at(framebuf_i)->setName(name)) {
      logE("uglue.descriptorSet[%zu] failed\n", framebuf_i);
      return 1;
    }

    auto& newSize = cpool.vk.dev.swapChainInfo.imageExtent;
    VkViewport& viewport = pipe0.info().viewports.at(0);
    viewport.width = (float)newSize.width;
    viewport.height = (float)newSize.height;
    pipe0.info().scissors.at(0).extent = newSize;
    VkBuffer vertBufs[] = {uglue.vertexBuffer.vk};
    VkDeviceSize offsets[] = {0};

    // Fill framebuffer with solid (alpha 1) or transparent (alpha 0):
    pipe0.pipe->clearColors.at(0).color.float32[3] =
        (fs.canDrawTransparent(uglue.window) && userTransparentRequest) ? 0 : 1;

    auto& cmdBuffer = uglue.cmdBuffers.at(framebuf_i);
    if (cmdBuffer.beginSimultaneousUse() ||
        cmdBuffer.beginSubpass(pass, framebuf, 0) ||
        cmdBuffer.bindGraphicsPipelineAndDescriptors(
            *pipe0.pipe, 0, 1, &uglue.descriptorSet.at(framebuf_i)->vk) ||
        cmdBuffer.setViewport(0, 1, &viewport) ||
        cmdBuffer.setScissor(0, 1, &pipe0.info().scissors.at(0)) ||
        cmdBuffer.bindVertexBuffers(0, sizeof(vertBufs) / sizeof(vertBufs[0]),
                                    vertBufs, offsets) ||
        cmdBuffer.bindAndDraw(uglue.indices, uglue.indexBuffer.vk,
                              0 /*indexBufOffset*/)) {
      logE("buildFramebuf: command buffer [%zu] failed\n", framebuf_i);
      return 1;
    }
    return uglue.endRenderPass(cmdBuffer, framebuf_i);
  }

  // updateGUI displays all the different fullscreen options in a GUI.
  int updateGUI() {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(64, 64), ImGuiCond_FirstUseEver);
    ImGui::Begin("Config");
    auto& dev = cpool.vk.dev;
    wantNormal = false;
    wantMax = 0;
    wantMonitor = nullptr;

    if (ImGui::BeginTabBar("tab_bar_id", ImGuiTabBarFlags_NoTooltip)) {
      if (ImGui::BeginTabItem("window", NULL, 0)) {
        ImGui::Text("%.0ffps window:%ux%u", ImGui::GetIO().Framerate,
                    dev.swapChainInfo.imageExtent.width,
                    dev.swapChainInfo.imageExtent.height);
        wantNormal =
            ImGui::RadioButton("normal window", fs.isNormal(uglue.window));

        const char* transparentName = "composited";
#ifdef __APPLE__
        is10_7_max = fs.isLionMax(uglue.window);
        if (is10_7_max) {
          transparentName = "(meaningless) composited";
        }
        wantMax = ImGui::RadioButton("macOS 10.7 spaces fullscreen", is10_7_max)
                      ? 2
                      : wantMax;
#endif /*__APPLE__*/

        // Create one ImGui::RadioButton() for each monitor.
        auto it = fs.getMonitors().begin();
        if (fs.getMonitors().size() < 2) {
          if (ImGui::RadioButton(fs.getFullscreenType(),
                                 fs.isFullscreen(uglue.window))) {
            wantMonitor = it->mon;
          }
        } else {
          char text[512];
          for (size_t i = 0; it != fs.getMonitors().end(); i++, it++) {
            snprintf(text, sizeof(text), "%s on %zu:%s", fs.getFullscreenType(),
                     i, it->name.c_str());
            if (ImGui::RadioButton(
                    text, fs.isFullscreen(uglue.window) &&
                              fs.getMonitor(uglue.window) == it->mon)) {
              wantMonitor = it->mon;
            }
          }
        }

#ifndef __ANDROID__
        if (ImGui::RadioButton("maximized window",
                               fs.isMaximized(uglue.window))) {
          wantMax = 1;
        }
#endif
        ImGui::Separator();

        if (fs.canDrawTransparent(uglue.window)) {
          bool prev = userTransparentRequest;
          ImGui::Checkbox(transparentName, &userTransparentRequest);
          if (prev != userTransparentRequest) {
            uglue.needRebuild = true;
          }
        }

#ifdef __ANDROID__
        static bool keepScreenOn = false;  // Matches what glfwAndroidMain sets.
        bool want = keepScreenOn;
        ImGui::Checkbox("keep screen on", &want);
        if (want != keepScreenOn ||
            keepScreenOn != fs.isFullscreen(uglue.window)) {
          keepScreenOn = want;
          if (keepScreenOn) {
            ANativeActivity_setWindowFlags(glfwGetAndroidApp()->activity,
                                           AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
          } else {
            ANativeActivity_setWindowFlags(glfwGetAndroidApp()->activity, 0,
                                           AWINDOW_FLAG_KEEP_SCREEN_ON);
          }
        }
#endif /* __ANDROID__ */
#ifdef VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME
        ImGui::Separator();
        ImGui::Text("VK_EXT_full_screen_exclusive:");
        VkFullScreenExclusiveEXT wantExclusiveMode = curExclusiveMode;
        for (int i = VK_FULL_SCREEN_EXCLUSIVE_BEGIN_RANGE_EXT;
             i <= VK_FULL_SCREEN_EXCLUSIVE_END_RANGE_EXT; i++) {
          auto mode = static_cast<VkFullScreenExclusiveEXT>(i);
          const char* name = string_VkFullScreenExclusiveEXT(mode);
          if (!strncmp(name, "VK_FULL_SCREEN_EXCLUSIVE_", 25)) {
            name += 25;
          }
          bool supported = hasExtExclusive || true;
          if (!supported) {  // Show unsupported modes as disabled.
            ImGui::Text("   %s", name);
            continue;
          }
          if (ImGui::RadioButton(name, i == curExclusiveMode)) {
            wantExclusiveMode = static_cast<VkFullScreenExclusiveEXT>(i);
          }
        }
        if (wantExclusiveMode != curExclusiveMode) {
          if (cpool.deviceWaitIdle()) {
            logE("wantExclusiveMode %d: cpool.deviceWaitIdle() failed\n",
                 (int)wantExclusiveMode);
            return 1;
          }
          cpool.vk.dev.destroySurface();
          logW("FIXME: mode change not implemented yet. Also get current.\n");
          curExclusiveMode = wantExclusiveMode;
          if (glfwCreateWindowSurface(instance.vk, uglue.window,
                                      instance.pAllocator, &instance.surface)) {
            return 1;
          }
          uglue.needRebuild = true;
        }
#endif /* VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME */
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("vsync", NULL, 0)) {
        ImGui::Text("%.0ffps window:%ux%u", ImGui::GetIO().Framerate,
                    dev.swapChainInfo.imageExtent.width,
                    dev.swapChainInfo.imageExtent.height);

        int wantMode = -1;
        for (int i = VK_PRESENT_MODE_BEGIN_RANGE_KHR;
             i <= VK_PRESENT_MODE_END_RANGE_KHR; i++) {
          bool supported = false;
          auto mode = static_cast<VkPresentModeKHR>(i);
          for (size_t j = 0; j < dev.presentModes.size(); j++) {
            if (dev.presentModes.at(j) == mode) {
              supported = true;
              break;
            }
          }
          const char* name = string_VkPresentModeKHR(mode);
          if (!strncmp(name, "VK_PRESENT_MODE_", 16)) {
            name += 16;
          }
          if (!supported) {  // Show unsupported modes as disabled.
            ImGui::Text("   %s", name);
            continue;
          }
          if (ImGui::RadioButton(name, i == dev.swapChainInfo.presentMode)) {
            wantMode = i;
          }
        }
        if (wantMode != -1) {
          dev.swapChainInfo.presentMode = (VkPresentModeKHR)wantMode;
          uglue.needRebuild = true;
        }
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

    ImGui::End();
    return 0;
  }

  int redraw(std::shared_ptr<memory::Flight>& flight) {
    if (updateGUI()) {
      return 1;
    }

    auto& ubo = *reinterpret_cast<UniformBufferObject*>(flight->mmap());
    ubo.model = glm::rotate(glm::rotate(glm::mat4(1.0f), glm::radians(40.0f),
                                        glm::vec3(0.0f, 1, 0)),
                            uglue.elapsed.get() * glm::radians(90.0f),
                            glm::vec3(0.0f, 0, 1));

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

 public:
  int run() {
    // Customize Dear ImGui with a larger font size.
    uglue.fonts.at(0)->SizePixels = 18.f;
    if (cpool.ctorError() || uglue.imGuiInit() || buildPass() ||
        uglue.descriptorLibrary.setName("uglue.descriptorLibrary")) {
      return 1;
    }
    // Poll monitors:
    onGLFWmonitorChange(NULL, 0);

    while (!uglue.windowShouldClose()) {
      UniformGlue::onGLFWRefresh(uglue.window);  // Calls redraw().
      if (uglue.redrawErrorCount > 0) {
        return 1;
      }

#ifdef __APPLE__
      if (!wantNormal && wantMax == 2) {
        fs.setLionMax(uglue.window, true);
      } else if (wantNormal || wantMax || wantMonitor) {
        // macOS must get out of LionMax mode before starting a different mode.
        fs.setLionMax(uglue.window, false);
      }
#endif /*__APPLE__*/

      if (wantNormal || wantMax) {
        fs.setFullscreen(uglue.window, nullptr);
      } else if (wantMonitor) {
        fs.setFullscreen(uglue.window, wantMonitor);
      }

      if (wantNormal) {
        fs.setMaximized(uglue.window, false);
      } else if (wantMax == 1) {
        fs.setMaximized(uglue.window, true);
      }
    }
    return cpool.deviceWaitIdle();
  }
};

static int addDeviceExtensions(language::Instance& inst) {
  (void)inst;
#ifdef VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME
  bool hasEXTfullscreenExclusive = false;
  for (auto& ext : inst.devs.at(0)->availableExtensions) {
    if (!strcmp(ext.extensionName,
                VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME)) {
      hasEXTfullscreenExclusive = true;
    }
  }
  if (hasEXTfullscreenExclusive) {
    inst.devs.at(0)->requiredExtensions.push_back(
        VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
  }
#endif /*VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME*/
  return 0;
}

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
      addDeviceExtensions(inst) ||
      inst.open({(uint32_t)width, (uint32_t)height})) {
    logE("inst.ctorError or enabledFeatures or inst.open failed\n");
  } else if (!inst.devs.size()) {
    logE("No vulkan devices found (or driver missing?)\n");
  } else {
    r = std::make_shared<Example09>(inst, window)->run();
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
  glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
  GLFWwindow* window = glfwCreateWindow(
      INIT_WIDTH, INIT_HEIGHT, "09fullscreen Vulkan window",
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
#define FLAG_TRANSLUCENT_NAVIGATION (0x08000000)
  ANativeActivity_setWindowFlags(
      app->activity, AWINDOW_FLAG_SHOW_WALLPAPER | FLAG_TRANSLUCENT_NAVIGATION,
      0);
  glfwAndroidMain(app, example::crossPlatformMain);
}
#else
// Posix startup.
int main(int argc, char** argv) {
  return example::crossPlatformMain(argc, argv);
}
#endif
