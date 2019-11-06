/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 */

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "../src/uniformglue/uniformglue.h"

// uniform_glue.h has already #included some glm headers.
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "../src/asset/asset.h"
#include "13instancing/13inst-buf.vert.h"
#include "13instancing/13inst-ubo.vert.h"
#include "13instancing/13instancing.frag.h"
#include "imgui.h"

#ifdef __ANDROID__
#include <android/keycodes.h>
#endif /*__ANDROID__*/

using namespace std;
namespace example {

namespace vert_buf {
#include "13instancing/struct_13inst-buf.vert.h"
}
using vert_buf::st_13inst_buf_vert;
namespace vert_ubo {
#include "13instancing/struct_13inst-ubo.vert.h"
}
using vert_ubo::st_13inst_ubo_vert;
namespace frag {
#include "13instancing/struct_13instancing.frag.h"
}

static constexpr size_t sizeofOneIndir =
    (sizeof(VkDrawIndexedIndirectCommand) + 3) & (~3);
static constexpr size_t maxInstPerUBO =
    sizeof(vert_ubo::UniformBufferObject::inst) /
    sizeof(vert_ubo::UniformBufferObject::inst[0]);

static size_t getMmapMaxUsingTemporaryVariable(command::CommandPool& cpool) {
  // This does not reflect the value in uglue.stage, below, but since
  // uglue is not constructed yet, this at least uses the same initializer.
  memory::Stage stage{cpool, memory::ASSUME_POOL_QINDEX};
  return stage.mmapMax();
}

class Example13 : public BaseApplication {
 public:
  Example13(language::Instance& instance, GLFWwindow* window, size_t gpuUboSize)
      : BaseApplication{instance},
        uboSize{getMmapMaxUsingTemporaryVariable(cpool)},
        gpuUboSize{gpuUboSize},
        // maxUBOs is one less than the absolute max that would fit in uboSize,
        // which leaves room for the indirs in that space.
        maxUBOs{uboSize / gpuUboSize - 1},
        maxIndirs{std::min((uboSize - maxUBOs * gpuUboSize) / sizeofOneIndir,
                           (size_t)maxUBOs)},
        uglue{*this, window, 0 /*maxLayoutIndex*/,
              vert_ubo::bindingIndexOfUniformBufferObject(),
              // Create a single large uniform buffer
              uboSize},
        assetLib{uglue} {
    resizeFramebufListeners.emplace_back(std::make_pair(
        [](void* self, language::Framebuf& framebuf, size_t fbi,
           size_t) -> int {
          return static_cast<Example13*>(self)->buildFramebuf(framebuf, fbi);
        },
        this));
    uglue.redrawListeners.emplace_back(std::make_pair(
        [](void* self, std::shared_ptr<memory::Flight>& flight) -> int {
          return static_cast<Example13*>(self)->redraw(flight);
        },
        this));
    uglue.keyEventListeners.push_back(std::make_pair(
        [](void* self, int key, int /*scancode*/, int action,
           int /*mods*/) -> void {
          if (action != GLFW_PRESS && action != GLFW_REPEAT) {
            return;
          }
          static_cast<Example13*>(self)->onModelRotate(
              key == GLFW_KEY_LEFT ? -10 : (key == GLFW_KEY_RIGHT ? 10 : 0),
              key == GLFW_KEY_UP ? -10 : (key == GLFW_KEY_DOWN ? 10 : 0));
          static_cast<Example13*>(self)->onMove(
              key == GLFW_KEY_A ? 1 : (key == GLFW_KEY_D ? -1 : 0),
              key == GLFW_KEY_R ? 1 : (key == GLFW_KEY_F ? -1 : 0),
              key == GLFW_KEY_W ? 1 : (key == GLFW_KEY_S ? -1 : 0));
        },
        this));
    uglue.inputEventListeners.push_back(std::make_pair(
        [](void* self, GLFWinputEvent* e, size_t eCount, int m,
           int enter) -> void {
          static_cast<Example13*>(self)->onInputEvent(e, eCount, m, enter);
        },
        this));
    if (vert_ubo::bindingIndexOfUniformBufferObject() !=
        vert_buf::bindingIndexOfUniformBufferObject()) {
      logE("This sample assumes both UBOs have the same binding.\n");
      logE("Got vert_ubo::bindingIndexOfUniformBufferObject() = %u\n",
           vert_ubo::bindingIndexOfUniformBufferObject());
      logF("Got vert_buf::bindingIndexOfUniformBufferObject() = %u\n",
           vert_buf::bindingIndexOfUniformBufferObject());
    }
    if (sizeof(InstBufLayout) != sizeof(st_13inst_buf_vert) -
          sizeof(st_13inst_ubo_vert)) {
      logE("This sample assumes InstBufLayout captures the instance\n");
      logE("inputs that are in 13inst-buf.vert\n");
      logE("Got InstBufLayout = %zu, 13inst-buf.vert = %zu,\n",
           sizeof(InstBufLayout), sizeof(st_13inst_buf_vert));
      logF("    13inst-ubo.vert = %zu\n", sizeof(st_13inst_ubo_vert));
    }
    if (sizeof(vert_buf::UniformBufferObject) !=
        offsetof(vert_ubo::UniformBufferObject, inst)) {
      logE("This sample assumes vert_ubo::UniformBufferObject (%zu)\n",
           offsetof(vert_ubo::UniformBufferObject, inst));
      logF("is laid out the same as vert_buf::UniformBufferObject (%zu)\n",
           sizeof(vert_buf::UniformBufferObject));
    }
  }

  enum InstancingMethods {
    INVALIDInstancingMethod = 0,
    UBO,
    Buf,
  };

  InstancingMethods instMethod{UBO};

  const size_t uboSize;
  const size_t gpuUboSize;
  const size_t maxUBOs;
  const size_t maxIndirs;

 protected:
  UniformGlue uglue;

  // orient represents rotation of the view matrix as a rotation quaternion.
  glm::quat orient = glm::angleAxis(0.f, glm::vec3(0, 1, 0));
  static constexpr size_t bindingIndexOfInstanceBuf = 1;
  static constexpr float deadzone = 0.1;
  static constexpr size_t maxIndices = 1024u * 1024u;

  asset::Library assetLib;

  typedef struct InstanceData {
    glm::vec4 loc;
    glm::vec4 vel;  // velocity
    glm::quat rot;
    glm::quat dr;  // angular velocity
  } InstanceData;

  std::vector<InstanceData> instance;
  memory::Buffer instBuf{cpool.vk.dev};

  typedef struct InstBufLayout {
    glm::vec4 loc;
    glm::vec4 rot;
  } InstBufLayout;
  InstBufLayout* instBufMmap{nullptr};

  static constexpr float maxLoc = 20;
  const float simSpeed = 1.f / 1024.f;

  glm::vec3 cam{0.f, 0.f, -maxLoc};
  vector<science::PipeBuilder> pipes;
  size_t activePipe{0};

  bool drawWire{false};

  // instRand returns a random value [0, 1.]
#ifdef _WIN32
  inline int rand_r(unsigned* seed) {
    (void)seed;  // rand_r() is not posix, not present in Windows.
    return rand();
  }
#endif /*_WIN32*/
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
    i.vel = glm::vec4(instRand(), instRand(), instRand(), 0.f) *
            simSpeed;
    i.rot = glm::angleAxis(0.f, glm::vec3(0, 0, 1));
    i.dr = glm::angleAxis(instRand() * simSpeed,
                          glm::vec3(instRand(), instRand(), instRand()));
  }

  void updateInstances() {
    for (size_t i = 0; i < instance.size(); i++) {
      auto& inst = instance.at(i);
      inst.loc += inst.vel;
      for (size_t j = 0; j < 3; j++) {
        if (inst.loc[j] < -maxLoc) {
          inst.vel[j] = fabs(inst.vel[j]);
          inst.loc[j] = -maxLoc;
        } else if (inst.loc[j] > maxLoc) {
          inst.vel[j] = -fabs(inst.vel[j]);
          inst.loc[j] = maxLoc;
        }
      }
      inst.rot = glm::normalize(inst.dr * inst.rot);
    }
  }

  void resetInstances() {
    for (size_t i = 0; i < instance.size(); i++) {
      auto loc = glm::vec4(i & 63, (i >> 6) & 63, -float(i >> 12), 0.f);
      loc *= 0.4f;
      loc += glm::vec4(-12.7f, -12.7f, 12.7f, 1.f);
      instance.at(i).loc = loc;
    }
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
    // Ask uglue to alias uniform buffers for use also for
    // VkDrawIndexedIndirectCommand in redraw(). Buffer layout is:
    // 0:              Uniform Buffer[0]
    // gpuUboSize:     Uniform Buffer[1] using dynamic uniform buffers
    // gpuUboSize * 2: Uniform Buffer[2]
    // ...
    // gpuUboSize * (maxUBOs - 1): Uniform Buffer[maxUBOs - 1]
    //                              (last Uniform Buffer)
    // gpuUboSize * maxUBOs: VkDrawIndexedIndirectCommand[0]
    //
    // The VkDrawIndexedIndirectCommand is aliased to the same buffer so
    // they are both updated with the same memory::Flight in redraw().
    uglue.uniformUsageBits |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

    while (uglue.stage.sources.size() < 3) {
      uglue.stage.sources.emplace_back(cpool);
    }

    while (instance.size() < 1024) {
      randomInstance();
    }

    instBuf.info.size = uglue.stage.mmapMax();
    instBuf.info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    auto vertUBOShader = std::make_shared<command::Shader>(cpool.vk.dev);
    auto vertBufShader = std::make_shared<command::Shader>(cpool.vk.dev);
    auto fragmentShader = make_shared<command::Shader>(cpool.vk.dev);
    if (vertUBOShader->loadSPV(spv_13inst_ubo_vert,
                               sizeof(spv_13inst_ubo_vert)) ||
        vertBufShader->loadSPV(spv_13inst_buf_vert,
                               sizeof(spv_13inst_buf_vert)) ||
        fragmentShader->loadSPV(spv_13instancing_frag,
                                sizeof(spv_13instancing_frag))) {
      logE("shader load failed\n");
      return 1;
    }
    if (assetLib.ctorError<st_13inst_buf_vert, InstBufLayout>(
            maxIndices, bindingIndexOfInstanceBuf) ||
        instBuf.ctorAndBindHostVisible()) {
      logE("buildPass: assetLib.ctorError or instBuf failed\n");
      return 1;
    }
    void* voidMmap;
    if (instBuf.mem.mmap(&voidMmap)) {
      logE("buildPass: instBuf.mem.mmap failed\n");
      return 1;
    }
    instBufMmap = reinterpret_cast<decltype(instBufMmap)>(voidMmap);

    if (cpool.setName("cpool") || pass.setName("pass") ||
        cpool.vk.dev.setName("cpool.vk.dev") ||
        cpool.vk.dev.setSurfaceName("inst.surface") ||
        cpool.vk.dev.swapChain.setName("cpool.vk.dev.swapChain") ||
        vertUBOShader->setName("vertUBOShader") ||
        vertBufShader->setName("vertBufShader") ||
        fragmentShader->setName("fragmentShader") ||
        uglue.renderSemaphore.setName("uglue.renderSemaphore") ||
        uglue.imageAvailableSemaphore.setName("imageAvailableSemaphore") ||
        uglue.renderDoneFence.setName("uglue.renderDoneFence") ||
        instBuf.setName("instBuf")) {
      logE("buildPass: cpool or pass or uglue.*.setName failed\n");
      return 1;
    }
    // Call ctorError on uglue classes because this class will *not* be calling
    // uglue.initPipeBuilderFrom() - because this uses asset::Library.
    if (uglue.imageAvailableSemaphore.ctorError() ||
        uglue.renderDoneFence.ctorError() ||
        uglue.renderSemaphore.ctorError()) {
      logE("UniformGlue::semaphore or fence.ctorError failed\n");
      return 1;
    }

    // remember what subpass index the pipelines will execute at.
    size_t theSubpass = pass.pipelines.size();

    // Create all PipeBuilders
    for (size_t i = 0; i < 4; i++) {
      pipes.emplace_back(pass);
      auto& thePipe = pipes.back();
      if (i == 0) {
        // Calling thePipe.info() registers thePipe with pass and increments
        // the subPass count in pass.
        auto& dynamicStates = thePipe.info().dynamicStates;
        dynamicStates.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
        dynamicStates.emplace_back(VK_DYNAMIC_STATE_SCISSOR);

        if (thePipe.addDepthImage({
                // These depth image formats will be tried in order:
                VK_FORMAT_D32_SFLOAT,
                VK_FORMAT_D32_SFLOAT_S8_UINT,
                VK_FORMAT_D24_UNORM_S8_UINT,
            })) {
          logE("pipe[%zu]: addDepthImage or addVertexInput failed\n", i);
          return 1;
        }
      } else {
        // This will *not* increment the subPass count.
        thePipe.deriveFrom(pipes.at(0));
      }
    }

    // Apply different shaders and specialization constants to each PipeBuilder
    for (size_t i = 0; i < pipes.size(); i++) {
      bool isBuf = !!(i & 1);
      bool isWire = !!(i & 2);

      auto& thePipe = pipes.at(i);
      if (uglue.shaders.add(thePipe, isBuf ? vertBufShader : vertUBOShader) ||
          uglue.shaders.add(thePipe, fragmentShader)) {
        logE("pipe[%zu] shaders failed\n", i);
        return 1;
      }

      frag::SpecializationConstants spec;
      spec.SHADING_MODE =
          isWire ? 2/*2 means wireframe - defined in frag shader*/ : 0;
      if (thePipe.info().specialize(spec)) {
        logE("specialize[%zu] failed\n", i);
        return 1;
      }

      const char* lineMode = "";
      if (isWire) {
        if (cpool.vk.dev.enabledFeatures.features.fillModeNonSolid) {
          thePipe.info().rastersci.polygonMode = VK_POLYGON_MODE_LINE;
          if (cpool.vk.dev.enabledFeatures.features.wideLines) {
            thePipe.info().dynamicStates.emplace_back(
                VK_DYNAMIC_STATE_LINE_WIDTH);
          }
          lineMode = " MODE_LINE";
        } else {
          lineMode = " MODE_LINE (disabled)";
        }
      }
      char name[256];
      snprintf(name, sizeof(name), "pipe[%zu] %s%s", i,
               isBuf ? "vertBuf" : "vertUBO", lineMode);
      if (thePipe.setName(name)) {
        logE("%s setName failed\n", name);
        return 1;
      }
      snprintf(name + strlen(name), sizeof(name) - strlen(name), " layout");
      if (thePipe.pipe->pipelineLayout.setName(name)) {
        logE("%s setName failed\n", name);
        return 1;
      }
    }
    if (pipes.at(0).addVertexInput<st_13inst_ubo_vert>() ||
        uglue.setDynamicUniformBuffer(gpuUboSize) ||
        uglue.buildPassAndTriggerResize()) {
      logE("setDynamicUniformBuffer or buildPassAndTriggerResize failed\n");
      return 1;
    }

    for (size_t i = 1; i < pipes.size(); i++) {
      bool isBuf = !!(i & 1);
      if (isBuf) {
        if (assetLib.addVertexAndInstInputs<st_13inst_buf_vert>(pipes.at(i))) {
          logE("pipe[%zu].addVertexAndInst...st_13inst_buf_vert> failed\n", i);
        }
      } else {
        if (pipes.at(i).addVertexInput<st_13inst_ubo_vert>()) {
          logE("pipe[%zu].addVertexInput<st_13inst_ubo_vert> failed\n", i);
          return 1;
        }
      }

      // Call pipes[*].ctorError for the other pipelines besides pipes[0].
      // Below, pipes.at(*).swap(pipes.at(*)) switches which one is active.
      if (pipes.at(i).pipe->ctorError(pass, theSubpass)) {
        logE("pipe[%zu].ctorError failed\n", i);
        return 1;
      }
    }
    return 0;
  }

  int bindUBOandDS(size_t uboOffset, size_t framebuf_i) {
    std::vector<uint32_t> dynamicUBO;
    dynamicUBO.emplace_back(uboOffset);
    if (uglue.cmdBuffers.at(framebuf_i).bindDescriptorSets(
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pass.pipelines.at(0)->pipelineLayout,
        0 /*firstSet*/, 1 /*descriptorSetCount*/,
        &uglue.descriptorSet.at(framebuf_i)->vk,
        // Update dynamic UBO with the location of the current UBO
        dynamicUBO.size(), dynamicUBO.data())) {
      logE("bindUBOandDS(%zu): bindDescriptorSets failed\n", uboOffset);
      return 1;
    }
    return 0;
  }

  int buildFramebuf(language::Framebuf& framebuf, size_t framebuf_i) {
    size_t wantPipe =
        ((instMethod == Buf ? 1 : 0)) + (drawWire ? 2 : 0);
    if (wantPipe != activePipe) {
      // NOTE: swap() here updates the RenderPass pass, but does not change the
      // order of the pipes in the vector 'pipes'.
      if (pipes.at(wantPipe).swap(pipes.at(activePipe))) {
        logE("pipes[%zu].swap(pipes[%zu]) failed\n", wantPipe, activePipe);
        return 1;
      }
      activePipe = wantPipe;
    }

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

    auto& newSize = cpool.vk.dev.swapChainInfo.imageExtent;
    VkViewport& view = pass.pipelines.at(0)->info.viewports.at(0);
    view.width = float(newSize.width);
    view.height = float(newSize.height);
    auto& scis = pass.pipelines.at(0)->info.scissors.at(0);
    scis.extent = newSize;

    auto& enabledFeats = cpool.vk.dev.enabledFeatures.features;
    if (cmdBuffer.beginSubpass(pass, framebuf, 0) ||
        // instead of bindGraphicsPipelineAndDescriptors(), separate binding
        // of pipeline and descriptors:
        cmdBuffer.bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS,
                               *pass.pipelines.at(0)) ||
        bindUBOandDS(0 /*uboOffset*/, framebuf_i) ||
        cmdBuffer.setViewport(0, 1, &view) ||
        cmdBuffer.setScissor(0, 1, &scis) ||
        // call setLineWidth only if enabled.
        (drawWire && enabledFeats.wideLines && cmdBuffer.setLineWidth(2.0f)) ||
        assetLib.bind(cmdBuffer) ||
        (instMethod == Buf &&
         assetLib.bindInstBuf(cmdBuffer, instBuf.vk, 0))) {
      logE("buildFramebuf(%zu): assetLib.bind failed\n", framebuf_i);
      return 1;
    }
    for (size_t i = 0; i < maxIndirs; i++) {
      // bindUBOandDS() does the dynamic uniform buffer binding. It is not
      // necessary to re-bind the uniform buffer the first time, since it was
      // just done above.
      if (i != 0 && bindUBOandDS(gpuUboSize * i, framebuf_i)) {
        logE("buildFramebuf(%zu): bindUBOandDS(%zu) failed", framebuf_i,
             gpuUboSize * i);
        return 1;
      }
      if (cmdBuffer.drawIndexedIndirect(
              uglue.uniform.at(framebuf_i).vk,
              maxUBOs * gpuUboSize + sizeofOneIndir * i /*offset*/,
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
    ImGui::SetNextWindowSize(ImVec2(196, 115));
    static constexpr int NonWindow =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground;
    ImGui::Begin("FPS", NULL, NonWindow);
    ImGui::Text("%.0ffps", ImGui::GetIO().Framerate);
    ImGui::Text("%zu inst %.1fMvert", instance.size(),
                float(instance.size()) * test1->verts() / 1e6);
    float sliderWidth = ImGui::GetWindowWidth() - ImGui::GetFontSize();
    int guiInstCount = instance.size();
    ImGui::PushItemWidth(sliderWidth);
    ImGui::SliderInt("", &guiInstCount, 1024, maxInstPerUBO * maxIndirs);
    ImGui::PopItemWidth();
    if ((size_t)guiInstCount < instance.size()) {
      instance.resize(guiInstCount);
    } else
      while ((size_t)guiInstCount > instance.size()) {
        randomInstance();
      }

    ImGui::Text("Instance");
    ImGui::SameLine();
    bool wantMethodUBO = ImGui::RadioButton("UBO", instMethod == UBO);
    ImGui::SameLine();
    bool wantMethodBuf = ImGui::RadioButton("Buffer", instMethod == Buf);
    if (wantMethodUBO) {
      instMethod = UBO;
      uglue.needRebuild = true;
    } else if (wantMethodBuf) {
      instMethod = Buf;
      uglue.needRebuild = true;
    }

    if (cpool.vk.dev.enabledFeatures.features.fillModeNonSolid) {
      bool wireChecked = drawWire;
      ImGui::Checkbox("Wireframe", &wireChecked);
      if (wireChecked != drawWire) {
        drawWire = wireChecked;
        uglue.needRebuild = true;
      }
      ImGui::SameLine();
    }
    if (!ImGui::Button("Restack")) {
      // update CPU-side instance data. It is copied to the GPU in two
      // places - UBO below, and for Buf in updateInstBuf().
      updateInstances();
    } else {
      // update CPU-side instance data, but reset position
      resetInstances();
    }
    ImGui::End();

    static char prevdbg[256];
    char dbg[256];
    snprintf(dbg, sizeof(dbg), "%zu:", instance.size());

    // Write per-instance data to each dynamic UBO. Once all the per-instance
    // data is consumed, write instanceCount = 0 to the rest of the
    // VkDrawIndexedIndirectCommand.
    size_t instDone = 0;
    char* uboPtr = reinterpret_cast<char*>(flight->mmap());
    char* indirPtr = uboPtr + maxUBOs * gpuUboSize;
    for (size_t i = 0; i < maxIndirs; i++,
         indirPtr += sizeofOneIndir, uboPtr += gpuUboSize) {
      // Write to ubo for each i - each dynamic UBO offset will need a copy
      // of these inputs:
      auto& ubo = *reinterpret_cast<vert_ubo::UniformBufferObject*>(uboPtr);
      ubo.view = glm::mat4_cast(orient) *
                 glm::lookAt(cam + glm::vec3(0.0f, 0.0f, -1.0f),  // Look at.
                             cam,                           // Camera pose.
                             glm::vec3(0.0f, 1.0f, 0.0f));  // Up vector.

      ubo.lightPos = glm::vec4(0, 2, 1, maxInstPerUBO);

      ubo.proj = glm::perspective(glm::radians(45.0f),
                                  cpool.vk.dev.aspectRatio(), 0.1f, 100.0f);
      ubo.proj[1][1] *= -1;  // Convert from OpenGL to Vulkan by flipping Y.

      auto& indir = *reinterpret_cast<VkDrawIndexedIndirectCommand*>(indirPtr);
      indir = test1->inst.cmd;
      indir.instanceCount = 0;
      indir.firstInstance = 0;
      if (test1->state() != asset::READY || instDone >= instance.size()) {
        continue;
      }
      if (instMethod == UBO) {
        // Each indir gets up to maxInstPerUBO instances
        indir.instanceCount = instance.size() - instDone;
        if (indir.instanceCount > maxInstPerUBO) {
          indir.instanceCount = maxInstPerUBO;
        }

        // Write per-instance data to UBO
        for (size_t j = instDone, k = 0; k < indir.instanceCount; j++, k++) {
          ubo.inst[k].loc = instance.at(j).loc;
          auto rot = instance.at(j).rot;
          ubo.inst[k].rot = glm::vec4{rot.x, rot.y, rot.z, rot.w};
        }
        instDone += indir.instanceCount;
        if (indir.instanceCount && indir.instanceCount != maxInstPerUBO) {
          int l = strlen(dbg);
          snprintf(dbg + l, sizeof(dbg) - l,
                   " %zu batches + %zu in final batch", i,
                   (size_t)indir.instanceCount);
        }
      } else {
        // instMethod == Buf.
        if (i != 0) {
          // Only the first indir is needed. The rest have instanceCount = 0,
          // and are not needed at all.
          indir.instanceCount = 0;
          continue;
        }

        indir.instanceCount = instance.size();
      }
    }
    if (0 && strcmp(prevdbg, dbg)) {
      if (instance.size() == maxInstPerUBO * maxUBOs) {
        // dbg ends up pretty bare, fill it in a little
        logI("draw %s (limited by Stage::mmapMax) %zu full batches\n", dbg,
             maxUBOs);
      } else {
        logI("draw %s\n", dbg);
      }
    }
    strcpy(prevdbg, dbg);

    command::SubmitInfo info;
    if (assetLib.write<Example13, st_13inst_ubo_vert>(info,
            [](Example13* self, const asset::Vertex& vert,
               st_13inst_ubo_vert* dst) -> int {
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
    if (uglue.submit(flight, info)) {
      logE("uglue.submit failed\n");
      return 1;
    }
    return 0;
  }

  int updateInstBuf() {
    auto* dst = instBufMmap;
    for (size_t i = 0; i < instance.size(); i++, dst++) {
      dst->loc = instance.at(i).loc;
      auto rot = instance.at(i).rot;
      dst->rot = glm::vec4{rot.x, rot.y, rot.z, rot.w};
    }
#ifdef VOLCANO_DISABLE_VULKANMEMORYALLOCATOR
    VkMappedMemoryRange VkInit(range);
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    if (instBuf.mem.flush(std::vector<VkMappedMemoryRange>{range})) {
#else  /*VOLCANO_DISABLE_VULKANMEMORYALLOCATOR*/
    if (instBuf.mem.flush()) {
#endif /*VOLCANO_DISABLE_VULKANMEMORYALLOCATOR*/
      logE("instBuf.mem.flush failed\n");
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
    // Define an eval callback to set flags and color for test1
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
      if (updateInstBuf()) {
        return 1;
      }
      UniformGlue::onGLFWRefresh(uglue.window);  // Calls redraw().
      if (uglue.redrawErrorCount > 0) {
        return 1;
      }
#ifndef __ANDROID__
      // Non-Android requires joystick polling.
      onInputEvent(NULL, 0, 0, GLFW_TRUE);
#endif /*_ANDROID__*/
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
    auto& limits = inst.devs.at(0)->physProp.properties.limits;
    size_t gpuUboSize = limits.maxUniformBufferRange;
    if (gpuUboSize < 16384) {
      logE("maxUniformBufferRange: %zu too small\n", gpuUboSize);
      return 1;
    } else if (gpuUboSize > 65536) {
      // GLSL shaders can only access 65536 bytes, even if GPU allows more.
      gpuUboSize = 65536;
    }
    r = std::make_shared<Example13>(inst, window, gpuUboSize)->run();
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
  GLFWwindow* window = glfwCreateWindow(800, 600, "13instancing Vulkan window",
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
