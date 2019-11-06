/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 */

#include "../src/asset/asset.h"
#include "../src/uniformglue/uniformglue.h"

// uniform_glue.h has already #included some glm headers.
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "21physics/21scene.frag.h"
#include "21physics/21scene.vert.h"
#include "imgui.h"

#ifdef __ANDROID__
#include <android/keycodes.h>
#endif /*__ANDROID__*/

using namespace std;
namespace example {

#include "21physics/struct_21scene.vert.h"
namespace frag {
#include "21physics/struct_21scene.frag.h"
}

static const size_t numUBOBatches = 32;
#define maxUBOsize (65536)
#define maxInstPerUBO \
  (sizeof(UniformBufferObject::inst) / sizeof(UniformBufferObject::inst[0]))

size_t calcUniformSize() {
  return maxUBOsize * (numUBOBatches - 1) + sizeof(UniformBufferObject);
}

class Example21 : public BaseApplication {
 public:
  Example21(language::Instance& instance, GLFWwindow* window)
      : BaseApplication{instance},
        uglue{*this, window, 0 /*maxLayoutIndex*/,
              bindingIndexOfUniformBufferObject(), calcUniformSize()},
        assetLib{uglue} {
    resizeFramebufListeners.emplace_back(std::make_pair(
        [](void* self, language::Framebuf& framebuf, size_t fbi,
           size_t) -> int {
          return static_cast<Example21*>(self)->buildFramebuf(framebuf, fbi);
        },
        this));
    uglue.redrawListeners.emplace_back(std::make_pair(
        [](void* self, std::shared_ptr<memory::Flight>& flight) -> int {
          return static_cast<Example21*>(self)->redraw(flight);
        },
        this));
    uglue.keyEventListeners.push_back(std::make_pair(
        [](void* self, int key, int /*scancode*/, int action,
           int /*mods*/) -> void {
          if (action != GLFW_PRESS && action != GLFW_REPEAT) {
            return;
          }
          static_cast<Example21*>(self)->onModelRotate(
              key == GLFW_KEY_LEFT ? -10 : (key == GLFW_KEY_RIGHT ? 10 : 0),
              key == GLFW_KEY_UP ? -10 : (key == GLFW_KEY_DOWN ? 10 : 0));
          static_cast<Example21*>(self)->onMove(
              key == GLFW_KEY_A ? 1 : (key == GLFW_KEY_D ? -1 : 0),
              key == GLFW_KEY_R ? 1 : (key == GLFW_KEY_F ? -1 : 0),
              key == GLFW_KEY_W ? 1 : (key == GLFW_KEY_S ? -1 : 0));
        },
        this));
    uglue.inputEventListeners.push_back(std::make_pair(
        [](void* self, GLFWinputEvent* e, size_t eCount, int m,
           int enter) -> void {
          static_cast<Example21*>(self)->onInputEvent(e, eCount, m, enter);
        },
        this));
  }

 protected:
  UniformGlue uglue;
  // orient represents rotation of the view matrix as a rotation quaternion.
  glm::quat orient = glm::angleAxis(0.f, glm::vec3(0, 1, 0));
  static constexpr float deadzone = 0.1;
  static constexpr size_t maxIndices = 1024u * 1024u;
#define sizeofOneIndir ((sizeof(VkDrawIndexedIndirectCommand) + 3) & (~3))

  asset::Library assetLib;

  typedef struct InstanceData {
    glm::vec4 loc;
    glm::vec4 vel;  // velocity
    glm::quat rot;
    glm::quat dr;  // angular velocity
  } InstanceData;

  std::vector<InstanceData> instance;

  static constexpr float maxLoc = 20;
  const float simSpeed = 1.f / 32.f;

  glm::vec3 cam{0.f, 0.f, -maxLoc};

  // instRand returns a random value [0, 1.]
  unsigned instRandSeed{0};
  inline float instRand() {
    static const float invMax = 1.f / float(RAND_MAX);
    return float(rand_r(&instRandSeed)) * invMax;
  }

  void randomInstance() {
    instance.emplace_back();
    auto& i = instance.back();
    i.loc = glm::vec4(instRand(), instRand(), instRand(), 1.f) * (maxLoc * 2) +
            glm::vec4(-maxLoc, -maxLoc, -maxLoc, 0.f);
    i.vel = glm::normalize(glm::vec4(instRand(), instRand(), instRand(), 0.f)) *
            simSpeed;
    i.rot = glm::angleAxis(0.f, glm::vec3(0, 0, 1));
    i.dr = glm::angleAxis(instRand() * simSpeed,
                          glm::vec3(instRand(), instRand(), instRand()));
  }

  void updateInstance(InstanceData& i) {
    i.loc += i.vel;
    for (size_t j = 0; j < 3; j++) {
      if (i.loc[j] < -maxLoc) {
        i.vel[j] = fabs(i.vel[j]);
        i.loc[j] = -maxLoc;
      } else if (i.loc[j] > maxLoc) {
        i.vel[j] = -fabs(i.vel[j]);
        i.loc[j] = maxLoc;
      }
    }
    i.rot = glm::normalize(i.dr * i.rot);
  }

  void onMove(float dx, float dy, float dz) {
    if (!dx && !dy && !dz) {
      return;
    }
    cam.x += dx;
    cam.y += dy;
    cam.z += dz;
    logI("cam: %.1f %.1f %.1f\n", cam.x, cam.y, cam.z);
  }

  void onModelRotate(float dx, float dy) {
    if (!dx && !dy) {
      return;
    }
    static constexpr float perPx = 1.0f / 8;  // Rotation per pixel.
    auto rx = glm::angleAxis(glm::radians(dx * perPx), glm::vec3(0, -1, 0));
    auto ry = glm::angleAxis(glm::radians(dy * perPx), glm::vec3(-1, 0, 0));
    orient = glm::normalize(rx * ry * orient);
  }

  void onInputEvent(GLFWinputEvent* events, size_t eventCount, int /*mods*/,
                    int entered) {
    if (!entered) {
      return;
    }

    // Calculate the movement of all pointers with button 0 pressed.
    float dx = 0, dy = 0;
    for (size_t i = 0; i < eventCount; i++) {
      if ((events[i].buttons & 1) && i < uglue.prevInput.size() &&
          (uglue.prevInput.at(i).buttons & 1)) {
        dx += events[i].x - uglue.prevInput.at(i).x;
        dy += events[i].y - uglue.prevInput.at(i).y;
      }
#ifdef __ANDROID__
      if (events[i].inputDevice == GLFW_INPUT_JOYSTICK &&
          (events[i].action == GLFW_PRESS || events[i].action == GLFW_REPEAT)) {
        int count;
        const unsigned char* b = glfwGetJoystickButtons(events[i].num, &count);
        if (count > AKEYCODE_DPAD_RIGHT) {
          dy = dy - 10 * b[AKEYCODE_DPAD_UP] + 10 * b[AKEYCODE_DPAD_DOWN];
          dx = dx - 10 * b[AKEYCODE_DPAD_LEFT] + 10 * b[AKEYCODE_DPAD_RIGHT];
        }
      }
#endif /*__ANDROID__*/
    }
    for (int jid = 0; jid <= GLFW_JOYSTICK_LAST; jid++) {
      if (!glfwJoystickPresent(jid)) {
        continue;
      }
      int count;
      const float* axes = glfwGetJoystickAxes(jid, &count);
      if (!axes || count < 6) {
        continue;
      }
      if (fabsf(axes[2]) > deadzone) {
        dx += axes[2];
      }
      if (fabsf(axes[5]) > deadzone) {
        dy += axes[5];
      }
      if (count >= 12) {
        if (fabsf(axes[10]) > deadzone) {
          dx += axes[10];
        }
        if (fabsf(axes[11]) > deadzone) {
          dy += axes[11];
        }
      }
    }
    onModelRotate(dx, dy);
  }

  int buildPass() {
    // Ask uglue to construct uniform buffers that also can be used for
    // VkDrawIndexedIndirectCommand in redraw().
    uglue.uniformUsageBits |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

    while (uglue.stage.sources.size() < 3) {
      uglue.stage.sources.emplace_back(cpool);
    }

    while (instance.size() < 1024) {
      randomInstance();
    }

    vector<science::PipeBuilder> pipes;
    auto vertexShader = std::make_shared<command::Shader>(cpool.vk.dev);
    auto fragmentShader = make_shared<command::Shader>(cpool.vk.dev);
    if (vertexShader->loadSPV(spv_21scene_vert, sizeof(spv_21scene_vert)) ||
        fragmentShader->loadSPV(spv_21scene_frag, sizeof(spv_21scene_frag))) {
      logE("mesh or shader or texture load failed\n");
      return 1;
    }
    if (assetLib.ctorError<st_21scene_vert>(1024 * 1024, maxIndices)) {
      logE("buildPass: assetLib.ctorError failed\n");
      return 1;
    }
    if (cpool.setName("cpool") || pass.setName("pass") ||
        cpool.vk.dev.setName("cpool.vk.dev") ||
        cpool.vk.dev.setSurfaceName("inst.surface") ||
        cpool.vk.dev.swapChain.setName("cpool.vk.dev.swapChain") ||
        vertexShader->setName("vertexShader") ||
        fragmentShader->setName("fragmentShader") ||
        uglue.renderSemaphore.setName("uglue.renderSemaphore") ||
        uglue.imageAvailableSemaphore.setName("imageAvailableSemaphore") ||
        uglue.renderDoneFence.setName("uglue.renderDoneFence")) {
      logE("buildPass: cpool or pass or uglue.*.setName failed\n");
      return 1;
    }
    if (uglue.imageAvailableSemaphore.ctorError() ||
        uglue.renderDoneFence.ctorError() ||
        uglue.renderSemaphore.ctorError()) {
      logE("UniformGlue::semaphore or fence.ctorError failed\n");
      return 1;
    }
    for (int i = 0; i < 3; i++) {
      // Create a PipeBuilder for this fragmentShader.
      pipes.emplace_back(pass);
      auto& thePipe = pipes.back();
      if (i == 0) {
        // This will register thePipe with pass and increment the subPass count
        thePipe.info();

        if (thePipe.addDepthImage({
                // These depth image formats will be tried in order:
                VK_FORMAT_D32_SFLOAT,
                VK_FORMAT_D32_SFLOAT_S8_UINT,
                VK_FORMAT_D24_UNORM_S8_UINT,
            }) ||
            thePipe.addVertexInput<st_21scene_vert>()) {
          logE("initPipeBuilderFrom: addDepthImage or addVertexInput failed\n");
          return 1;
        }
      } else {
        // This will *not* increment the subPass count.
        thePipe.deriveFrom(pipes.at(0));
      }
      if (uglue.shaders.add(thePipe, vertexShader) ||
          uglue.shaders.add(thePipe, fragmentShader)) {
        logE("pipe[%d] shaders failed\n", i);
        return 1;
      }

      auto& dynamicStates = thePipe.info().dynamicStates;
      dynamicStates.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
      dynamicStates.emplace_back(VK_DYNAMIC_STATE_SCISSOR);

      frag::SpecializationConstants spec;
      spec.SHADING_MODE = i;
      if (thePipe.info().specialize(spec)) {
        logE("specialize[%d] failed\n", i);
        return 1;
      }
    }

    const char* name;
    if (cpool.vk.dev.enabledFeatures.features.fillModeNonSolid) {
      pipes.at(2).info().rastersci.polygonMode = VK_POLYGON_MODE_LINE;
      name = "pipe[2] MODE_LINE";
    } else {
      name = "pipe[2] MODE_LINE (not enabled)";
    }
    if (cpool.vk.dev.enabledFeatures.features.wideLines) {
      pipes.at(2).info().dynamicStates.emplace_back(
          VK_DYNAMIC_STATE_LINE_WIDTH);
    }
    if (pipes.at(0).setName("pipe[0] phong shading") ||
        pipes.at(0).pipe->pipelineLayout.setName("pipe[0] layout") ||
        pipes.at(1).setName("pipe[1] toon shading") ||
        pipes.at(1).pipe->pipelineLayout.setName("pipe[1] layout") ||
        pipes.at(2).setName(name) ||
        pipes.at(2).pipe->pipelineLayout.setName("pipe[2] layout")) {
      logE("pipe[*] setName failed\n");
      return 1;
    }
    return uglue.buildPassAndTriggerResize();
  }

  int buildFramebuf(language::Framebuf& framebuf, size_t framebuf_i) {
    char name[256];
    snprintf(name, sizeof(name), "uglue.uniform[%zu]", framebuf_i);
    if (uglue.uniform.at(framebuf_i).setName(name)) {
      logE("uglue.uniform[%zu].setName failed\n", framebuf_i);
      return 1;
    }

    auto& cmdBuffer = uglue.cmdBuffers.at(framebuf_i);
    if (cmdBuffer.beginSimultaneousUse()) {
      logE("buildFramebuf(%zu): beginSimultaneousUse failed\n", framebuf_i);
      return 1;
    }

    static const size_t pipeI = 0;
    auto& newSize = cpool.vk.dev.swapChainInfo.imageExtent;
    VkViewport& view = pass.pipelines.at(pipeI)->info.viewports.at(0);
    view.width = float(newSize.width);
    view.height = float(newSize.height);
    auto& scis = pass.pipelines.at(pipeI)->info.scissors.at(0);
    scis.extent = newSize;

    auto& enabledFeatures = cpool.vk.dev.enabledFeatures.features;
    if (cmdBuffer.beginSubpass(pass, framebuf, 0) ||
        cmdBuffer.bindGraphicsPipelineAndDescriptors(
            *pass.pipelines.at(pipeI), 0, 1,
            &uglue.descriptorSet.at(framebuf_i)->vk) ||
        cmdBuffer.setViewport(0, 1, &view) ||
        cmdBuffer.setScissor(0, 1, &scis) ||
        // call setLineWidth only if enabled and only for pipeline 2.
        (pipeI == 2 && enabledFeatures.wideLines &&
         cmdBuffer.setLineWidth(2.0f)) ||
        assetLib.bind(cmdBuffer, framebuf_i)) {
      logE("buildFramebuf(%zu): assetLib.bind failed\n", framebuf_i);
      return 1;
    }
    for (size_t i = 0; i < numUBOBatches; i++) {
      if (cmdBuffer.drawIndexedIndirect(
              uglue.uniform.at(framebuf_i).vk, maxUBOsize * i /*offset*/,
              1 /* drawCount: (cannot be >1 without multiDrawIndirect) */)) {
        logE("buildFramebuf(%zu): drawIndexedIndirect[%zu] failed\n",
             framebuf_i, i);
        return 1;
      }
    }
    return uglue.endRenderPass(cmdBuffer, framebuf_i);
  }

  int redraw(std::shared_ptr<memory::Flight>& flight) {
    onModelRotate(uglue.curJoyX, uglue.curJoyY);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(64, 64));
    ImGui::SetNextWindowSize(ImVec2(200, 80));
    static constexpr int NonWindow =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground;
    ImGui::Begin("FPS - also see VK_VERTEX_INPUT_RATE_INSTANCE", NULL,
                 NonWindow);
    ImGui::Text("%.0ffps", ImGui::GetIO().Framerate);
    ImGui::Text("%zu inst %.1fMvert", instance.size(),
                float(instance.size()) * test1->verts() / 1e6);
    float sliderWidth = ImGui::GetWindowWidth() - ImGui::GetFontSize();
    int guiInstCount = instance.size();
    ImGui::PushItemWidth(sliderWidth);
    ImGui::SliderInt("", &guiInstCount, 1024, numUBOBatches * maxInstPerUBO);
    ImGui::PopItemWidth();
    if ((size_t)guiInstCount < instance.size()) {
      instance.resize(guiInstCount);
    } else
      while ((size_t)guiInstCount > instance.size()) {
        randomInstance();
      }
    ImGui::End();

    static char prevdbg[256];
    char dbg[256];
    snprintf(dbg, sizeof(dbg), "%zu:", instance.size());
    size_t instDone = 0;
    char* mmap = reinterpret_cast<char*>(flight->mmap());
    for (size_t i = 0; i < numUBOBatches; i++, mmap += maxUBOsize) {
      auto& ubo = *reinterpret_cast<UniformBufferObject*>(mmap);
      auto& indir = *reinterpret_cast<VkDrawIndexedIndirectCommand*>(&ubo);
      indir.indexCount = assetLib.getIndicesUsed();
      indir.instanceCount = 0;
      if (test1->state() == asset::READY && instDone < instance.size()) {
        indir.instanceCount = instance.size() - instDone;
        if (indir.instanceCount > maxInstPerUBO) {
          indir.instanceCount = maxInstPerUBO;
        }
      }
      indir.firstIndex = 0;
      indir.vertexOffset = 0;
      indir.firstInstance = 0;

      ubo.view = glm::mat4_cast(orient) *
                 glm::lookAt(cam + glm::vec3(0.0f, 0.0f, -1.0f),  // Look at.
                             cam,                           // Camera pose.
                             glm::vec3(0.0f, 1.0f, 0.0f));  // Up vector.

      ubo.lightPos = glm::vec4(0, 2, 1, 0);

      ubo.proj = glm::perspective(glm::radians(45.0f),
                                  cpool.vk.dev.aspectRatio(), 0.1f, 100.0f);
      ubo.proj[1][1] *= -1;  // Convert from OpenGL to Vulkan by flipping Y.

      for (size_t j = instDone, k = 0; j < indir.instanceCount; j++, k++) {
        updateInstance(instance.at(j));
        ubo.inst[k].loc = instance.at(j).loc;
        auto rot = instance.at(j).rot;
        ubo.inst[k].rot = glm::vec4{rot.x, rot.y, rot.z, rot.w};
      }
      instDone += indir.instanceCount;
      if (indir.instanceCount && indir.instanceCount != maxInstPerUBO) {
        int l = strlen(dbg);
        snprintf(dbg + l, sizeof(dbg) - l, " %zu batches + %zu in final batch",
                 i, (size_t)indir.instanceCount);
      }
    }
    if (strcmp(prevdbg, dbg)) {
      if (instance.size() == maxInstPerUBO * numUBOBatches) {
        // dbg ends up pretty bare, fill it in a little
        logI("draw %s (limited by Stage::mmapMax) %zu full batches\n", dbg,
             numUBOBatches);
      } else {
        logI("draw %s\n", dbg);
      }
      strcpy(prevdbg, dbg);
    }

    command::SubmitInfo info;
    if (uglue.submit(flight, info)) {
      logE("uglue.submit failed\n");
      return 1;
    }
    return 0;
  }

 public:
  std::shared_ptr<asset::Revolv> test1;
  int run() {
    if (cpool.ctorError() || uglue.imGuiInit() || buildPass() ||
        uglue.descriptorLibrary.setName("uglue.descriptorLibrary")) {
      logE("cpool or buildPass failed\n");
      return 1;
    }

    // Generate a cylindrical solid
    const float test1scale = 0.1f;
    test1 = std::make_shared<asset::Revolv>();
    test1->rots = 32;
    test1->pt.emplace_back(glm::vec2(1.f, -1.f) * test1scale);
    test1->pt.emplace_back(glm::vec2(.7f, -.7f) * test1scale);
    test1->pt.emplace_back(glm::vec2(.7f, .7f) * test1scale);
    test1->pt.emplace_back(glm::vec2(1.f, 1.f) * test1scale);
    test1->eval = std::make_pair(
        [](void* userData, std::vector<asset::Vertex>& v,
           uint32_t& flags) -> int {
          (void)userData;
          for (size_t i = 0; i < v.size(); i++) {
            if (fabs(v.at(i).N.x) + fabs(v.at(i).N.z) < 0.1) {
              // sharp edge on cylinder caps
              flags |= asset::VERT_PER_FACE;
              break;
            }
          }
          for (size_t k = 0; k < v.size(); k++) {
            auto c = glm::vec4(glm::normalize(v.at(k).P), 1.f);
            if (c.z < 0.f) {
              c = glm::abs(c);
            } else {
              c = glm::clamp(c, 0.f, 1.f);
            }
            v.at(k).color = c;
          }
          return 0;
        },
        this);
    if (assetLib.add(test1)) {
      logE("assetLib.add(test1) failed\n");
      return 1;
    }

    // Begin main loop.
    while (!uglue.windowShouldClose()) {
      UniformGlue::onGLFWRefresh(uglue.window);  // Calls redraw().
      if (uglue.redrawErrorCount > 0) {
        return 1;
      }
#ifndef __ANDROID__
      // Non-Android requires joystick polling.
      onInputEvent(NULL, 0, 0, GLFW_TRUE);
#endif /*_ANDROID__*/
      if (assetLib.write<Example21, st_21scene_vert>(
              [](Example21* self, const asset::Vertex& vert,
                 st_21scene_vert* dst) -> int {
                (void)self;
                dst->inPosition = vert.P;
                dst->inNormal = vert.N;
                dst->inColor = glm::vec3(vert.color);
                return 0;
              },
              this)) {
        logE("assetLib.write failed\n");
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
      inst.devs.at(0)->enabledFeatures.set("fillModeNonSolid", VK_TRUE) ||
      inst.devs.at(0)->enabledFeatures.set("wideLines", VK_TRUE) ||
      // inst.open() takes a while, especially if validation layers are on.
      inst.open({(uint32_t)width, (uint32_t)height})) {
    logE("inst.ctorError or enabledFeatures or inst.open failed\n");
  } else if (!inst.devs.size()) {
    logE("No vulkan devices found (or driver missing?)\n");
  } else {
    r = std::make_shared<Example21>(inst, window)->run();
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
  GLFWwindow* window = glfwCreateWindow(800, 600, "21physics Vulkan window",
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
