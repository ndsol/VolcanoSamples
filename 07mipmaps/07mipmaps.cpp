/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This sample demonstrates loading a texture.
 */

#include "../src/load_gli.h"
#include "../src/uniformglue/uniformglue.h"

// uniform_glue.h has already #included some glm headers.
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <gli/gli.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "07mipmaps/07mipmaps.frag.h"
#include "07mipmaps/07mipmaps.vert.h"
#include "imgui.h"

namespace example {

#include "07mipmaps/struct_07mipmaps.vert.h"
std::vector<st_07mipmaps_vert> vertices;

namespace frag {
#include "07mipmaps/struct_07mipmaps.frag.h"
}

// Example07 is documented at github.com/ndsol/VolcanoSamples/07mipmaps/
class Example07 : public BaseApplication {
 public:
  Example07(language::Instance& instance, GLFWwindow* window)
      : BaseApplication{instance},
        uglue{*this, window, 0 /*maxLayoutIndex*/,
              bindingIndexOfUniformBufferObject(),
              sizeof(UniformBufferObject)} {
    resizeFramebufListeners.emplace_back(std::make_pair(
        [](void* self, language::Framebuf& framebuf, size_t fbi,
           size_t) -> int {
          return static_cast<Example07*>(self)->buildFramebuf(framebuf, fbi);
        },
        this));
    uglue.redrawListeners.emplace_back(std::make_pair(
        [](void* self, std::shared_ptr<memory::Flight>& flight) -> int {
          return static_cast<Example07*>(self)->redraw(flight);
        },
        this));
    uglue.keyEventListeners.push_back(std::make_pair(
        [](void* self, int key, int /*scancode*/, int action,
           int /*mods*/) -> void {
          if (action != GLFW_PRESS && action != GLFW_REPEAT) {
            return;
          }
          auto* c = static_cast<Example07*>(self);
          if (key == GLFW_KEY_M) {
            c->mipEnable = !c->mipEnable;
            c->uglue.needRebuild = true;
          } else if (key == GLFW_KEY_LEFT) {
            if (c->debugOneLod > -1) {
              c->debugOneLod--;
              c->uglue.needRebuild = true;
            }
          } else if (key == GLFW_KEY_RIGHT) {
            if (c->debugOneLod < c->sampleAndMip.info.maxLod) {
              c->debugOneLod++;
              c->uglue.needRebuild = true;
            }
          }
        },
        this));
    uglue.inputEventListeners.push_back(std::make_pair(
        [](void* self, GLFWinputEvent* e, size_t eCount, int m,
           int enter) -> void {
          static_cast<Example07*>(self)->onInputEvent(e, eCount, m, enter);
        },
        this));
  }

 protected:
  UniformGlue uglue;
  science::Sampler sampleAndMip{cpool.vk.dev};
  science::Sampler sampleNoMip{cpool.vk.dev};
  science::PipeBuilder pipe0{pass};
  int debugOneLod{-1};
  bool anisoEnable{false}, magLinear{true}, minLinear{false};
  bool mipEnable{true}, mipLinear{true};
  // cx, cy, cz are the camera position.
  float cx{0.7f}, cy{0.0f}, cz{-0.083f};

  int setSamplerFlags(science::Sampler& sampler) {
    // https://www.khronos.org/registry/vulkan/specs/1.0/man/html/VkSamplerCreateInfo.html
    sampler.info.anisotropyEnable = anisoEnable ? VK_TRUE : VK_FALSE;
    sampler.info.magFilter = magLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sampler.info.minFilter = minLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sampler.info.mipmapMode = mipLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                        : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    return 0;
  }

  int initTextureSampler(science::Sampler& sampler, bool andMip) {
    std::string textureFound;
    gli::texture gli_mem;
    if (loadGLI("zebra01.ktx", textureFound, gli_mem)) {
      return 1;
    }
    if (gli_mem.empty()) {
      logE("gli::load(%s) failed\n", textureFound.c_str());
      return 1;
    }
    gli::texture2d tex(gli_mem);
    if (tex.empty()) {
      logE("gli::texture2d(%s) failed\n", textureFound.c_str());
      return 1;
    }
    sampler.info.maxLod = andMip ? tex.levels() : 1;
    return setSamplerFlags(sampler) ||
           constructSamplerGLI(sampler, uglue.stage, tex);
  }

  int buildPass() {
    pipe0.info().rastersci.cullMode = VK_CULL_MODE_NONE;
    uint32_t n = 0;
    // Generate vertices and indices for a long flat plate with some
    // "raised tiles" to show motion.
    for (int y = -2; y < 3; y++) {
      for (int x = 0; x < 2000; x++, n += 4) {
        float xl = x - 0.5f, xh = x + 0.5f;
        float yl = y - 0.5f, yh = y + 0.5f;
        float z = (((x ^ y) & 7) == 3) * 0.01f;
        vertices.emplace_back(st_07mipmaps_vert{{xl, yl, z}, {0.0f, 1.0f}});
        vertices.emplace_back(st_07mipmaps_vert{{xh, yl, z}, {0.0f, 0.0f}});
        vertices.emplace_back(st_07mipmaps_vert{{xh, yh, z}, {-1.0f, 0.0f}});
        vertices.emplace_back(st_07mipmaps_vert{{xl, yh, z}, {-1.0f, 1.0f}});
        uglue.indices.insert(uglue.indices.end(),
                             {n, n + 1, n + 2, n + 2, n + 3, n});
      }
    }

    auto vert = std::make_shared<command::Shader>(cpool.vk.dev);
    auto frag = std::make_shared<command::Shader>(cpool.vk.dev);
    return vert->loadSPV(spv_07mipmaps_vert, sizeof(spv_07mipmaps_vert)) ||
           frag->loadSPV(spv_07mipmaps_frag, sizeof(spv_07mipmaps_frag)) ||
           uglue.shaders.add(pipe0, vert) || uglue.shaders.add(pipe0, frag) ||
           initTextureSampler(sampleAndMip, true) ||
           initTextureSampler(sampleNoMip, false) ||
           uglue.initPipeBuilderFrom(pipe0, vertices) ||
           uglue.buildPassAndTriggerResize();
  }

  int buildFramebuf(language::Framebuf& framebuf, size_t framebuf_i) {
    science::Sampler& sampler = mipEnable ? sampleAndMip : sampleNoMip;
    // Rebuild sampler. This only needs doing once for all framebufs.
    if (framebuf_i == 0 &&
        (setSamplerFlags(sampler) || sampler.ctorErrorNoImageViewInit())) {
      return 1;
    }
    // Populate descriptor set with the chosen sampler.
    if (uglue.descriptorSet.at(framebuf_i)
            ->write(frag::bindingIndexOftexSampler(),
                    std::vector<science::Sampler*>{&sampler})) {
      return 1;
    }

    auto& newSize = cpool.vk.dev.swapChainInfo.imageExtent;
    VkViewport& viewport = pipe0.info().viewports.at(0);
    viewport.width = float(newSize.width);
    viewport.height = float(newSize.height);
    pipe0.info().scissors.at(0).extent = newSize;
    VkBuffer vertBufs[] = {uglue.vertexBuffer.vk};
    VkDeviceSize offsets[] = {0};
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

  void onInputEvent(GLFWinputEvent* events, size_t eventCount, int /*mods*/,
                    int entered) {
    if (!entered) {
      return;
    }

    for (size_t i = 0; i < eventCount; i++) {
      // If only one pointer or finger touch, button #1 moves in y and x axes.
      if (eventCount == 1 && events[i].action == GLFW_CURSORPOS &&
          (events[i].buttons & 1)) {
        cy += events[i].dx * 0.005f;
        cx -= events[i].dy * 0.001f;
      }
      // Button #2 moves in the z axis. Two finger drag also.
      float dy = 0;
      if (eventCount == 1 && events[i].action == GLFW_CURSORPOS &&
          (events[i].buttons & 2)) {
        dy = events[i].dy;
      } else if (eventCount == 2 && i == 0 &&
                 events[i].action == GLFW_CURSORPOS &&
                 events[1].action == GLFW_CURSORPOS) {
        dy = (events[i].dy + events[1].dy) * 0.5f;
      }
      cz -= dy * 0.001f;
    }
  }

  int redraw(std::shared_ptr<memory::Flight>& flight) {
    cy += 0.01 * uglue.curJoyX;
    cx += 0.1 * uglue.curJoyY;
    auto& io = ImGui::GetIO();
    ImGui::NewFrame();
    // Decay to a straight, aligned view.
    if (!io.MouseDown[0]) {
      float logterm = log2f(fabs(cy));
      if (logterm > 0.0f) {
        cy *= 1.0f - 0.5f / io.Framerate;
      } else if (logterm > -10.0f && logterm < 0.0f) {
        cy *= 1.0f - 0.5f / io.Framerate + logterm / 2000;
      } else {
        cy = 0.0f;
      }
    }
    ImGui::SetNextWindowPos(ImVec2(64, 64));
    static constexpr int NonWindow = ImGuiWindowFlags_NoTitleBar |
                                     ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoInputs;
    ImGui::Begin("FPS", NULL, NonWindow);
    ImGui::Text("%.0ffps", io.Framerate);
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetFontSize() * 10, 64),
                            ImGuiCond_FirstUseEver);
    ImGui::Begin("Config");
    {
      bool prev = magLinear;
      ImGui::Checkbox("linear upsampling", &magLinear);
      uglue.needRebuild |= prev != magLinear;
      prev = minLinear;
      ImGui::Checkbox("linear downsampling", &minLinear);
      uglue.needRebuild |= prev != minLinear;
      prev = mipLinear;
      ImGui::Checkbox("linear mipmapping", &mipLinear);
      uglue.needRebuild |= prev != mipLinear;
      if (cpool.vk.dev.enabledFeatures.features.samplerAnisotropy) {
        prev = anisoEnable;
        ImGui::Checkbox("anisotropic filtering", &anisoEnable);
        uglue.needRebuild |= prev != anisoEnable;
      } else {
        ImGui::Text("aniso: device lacks this feature");
      }
      prev = mipEnable;
      int prevLod = debugOneLod;
      ImGui::Checkbox("mipmapping enabled", &mipEnable);
      ImGui::Text("show lod level in red:");
      ImGui::PushItemWidth(ImGui::GetWindowWidth());
      float v = debugOneLod;
      int maxLod = sampleAndMip.info.maxLod;
#ifdef __ANDROID__
      const char* sliderFmt = "%.1f";  // ImGui Gamepad speed bugfix.
#else                                  /*__ANDROID__*/
      const char* sliderFmt = "%.0f";
#endif                                 /*__ANDROID__*/
      ImGui::SliderFloat("", &v, -1, (float)maxLod - 0.51f, sliderFmt);
      if (v != debugOneLod) {
        debugOneLod = (int)(v < debugOneLod ? floor(v) : ceil(v));
      }
      debugOneLod = (debugOneLod >= maxLod) ? maxLod - 1 : debugOneLod;
      ImGui::PopItemWidth();
      uglue.needRebuild |= prev != mipEnable;
      uglue.needRebuild |= prevLod != debugOneLod;
    }
    ImGui::End();

    auto& ubo = *reinterpret_cast<UniformBufferObject*>(flight->mmap());
    ubo.model = glm::translate(glm::mat4(1.0f), glm::vec3(cx, 0.0, cz));

    ubo.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f),   // Object pose.
                           glm::vec3(1.0f, cy, 0.0f),     // Camera pose.
                           glm::vec3(0.0f, 0.0f, 1.0f));  // Up vector.

    ubo.proj = glm::perspective(glm::radians(45.0f), cpool.vk.dev.aspectRatio(),
                                0.1f, 1000.0f);
    ubo.proj[1][1] *= -1;  // Convert from OpenGL to Vulkan by flipping Y.

    ubo.debugOneLod = debugOneLod;

    command::SubmitInfo info;
    if (uglue.submit(flight, info)) {
      logE("uglue.submit failed\n");
      return 1;
    }
    return 0;
  }

 public:
  int run() {
    if (cpool.ctorError() || uglue.imGuiInit() || buildPass()) {
      return 1;
    }

    while (!uglue.windowShouldClose()) {
      UniformGlue::onGLFWRefresh(uglue.window);  // Calls redraw().
      if (uglue.redrawErrorCount > 0) {
        return 1;
      }
    }
    return cpool.deviceWaitIdle();
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
      inst.open({(uint32_t)width, (uint32_t)height})) {
    logE("inst.ctorError or enabledFeatures or inst.open failed\n");
  } else if (!inst.devs.size()) {
    logE("No vulkan devices found (or driver missing?)\n");
  } else {
    r = std::make_shared<Example07>(inst, window)->run();
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
  GLFWwindow* window = glfwCreateWindow(800, 600, "07mipmaps Vulkan window",
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
