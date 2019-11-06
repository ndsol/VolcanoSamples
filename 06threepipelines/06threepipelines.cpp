/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This sample demonstrates three pipelines used in a single render pass.
 */

#include "../src/assimpglue.h"
#include "../src/uniformglue/uniformglue.h"

// uniform_glue.h has already #included some glm headers.
#include <assimp/scene.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#define _USE_MATH_DEFINES /*Windows otherwise hides M_PI*/
#include <math.h>

#include "06threepipelines/06phong.frag.h"
#include "06threepipelines/06threepipelines.vert.h"
#include "06threepipelines/06toon.frag.h"
#include "06threepipelines/06wireframe.frag.h"

#ifdef __ANDROID__
#include <android/keycodes.h>
#endif /*__ANDROID__*/

using namespace std;
namespace example {

#include "06threepipelines/struct_06threepipelines.vert.h"

// Example06 is documented at github.com/ndsol/VolcanoSamples/06threepipelines/
class Example06 : public BaseApplication {
 public:
  Example06(language::Instance& instance, GLFWwindow* window)
      : BaseApplication{instance},
        uglue{*this, window, 0 /*maxLayoutIndex*/,
              bindingIndexOfUniformBufferObject(),
              sizeof(UniformBufferObject)} {
    resizeFramebufListeners.emplace_back(std::make_pair(
        [](void* self, language::Framebuf& framebuf, size_t fbi,
           size_t) -> int {
          return static_cast<Example06*>(self)->buildFramebuf(framebuf, fbi);
        },
        this));
    uglue.redrawListeners.emplace_back(std::make_pair(
        [](void* self, std::shared_ptr<memory::Flight>& flight) -> int {
          return static_cast<Example06*>(self)->redraw(flight);
        },
        this));
    uglue.keyEventListeners.push_back(std::make_pair(
        [](void* self, int key, int /*scancode*/, int action,
           int /*mods*/) -> void {
          if (action != GLFW_PRESS && action != GLFW_REPEAT) {
            return;
          }
          static_cast<Example06*>(self)->onModelRotate(
              key == GLFW_KEY_LEFT ? -10 : (key == GLFW_KEY_RIGHT ? 10 : 0),
              key == GLFW_KEY_UP ? -10 : (key == GLFW_KEY_DOWN ? 10 : 0));
        },
        this));
    uglue.inputEventListeners.push_back(std::make_pair(
        [](void* self, GLFWinputEvent* e, size_t eCount, int m,
           int enter) -> void {
          static_cast<Example06*>(self)->onInputEvent(e, eCount, m, enter);
        },
        this));
  }

 protected:
  // uglue simplifies a render loop by wrapping the uniform buffer boilerplate.
  UniformGlue uglue;
  // modelOrient represents the model matrix as a single rotation quaternion.
  glm::quat modelOrient =
      glm::angleAxis((float)M_PI * -.5f, glm::vec3(0, 0, 1));
  // widthSplit is the calculated width for each of the 3 viewports.
  int widthSplit{0};
  // vertices is the vertex buffer until initUniformAndPipeBuilder copies it to
  // the device. The vector<uint32_t> indices (the index buffer) is
  // conveniently provided by uglue.
  vector<st_06threepipelines_vert> vertices;
  static constexpr int NUM_PIPELINES = 3;
  static constexpr float deadzone = 0.1;

  void onModelRotate(float dx, float dy) {
    if (!dx && !dy) {
      return;
    }
    static constexpr float perPx = 1.0f / 8;  // Rotation per pixel.
    auto vx = glm::vec3(0, 1, 0);             // X motion rotates around Y axis.
    auto vy = glm::vec3(0, 0, 1);             // Y motion rotates around Z axis.
    auto rx = glm::angleAxis(glm::radians(dx * perPx), vx);
    auto ry = glm::angleAxis(glm::radians(dy * perPx), vy);
    modelOrient = glm::normalize(rx * ry * modelOrient);
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
      if (!axes || count < 2) {
        continue;
      }
      if (fabsf(axes[0]) > deadzone) {
        dx += axes[0];
      }
      if (fabsf(axes[1]) > deadzone) {
        dy += axes[1];
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

  // updateWidthSplit recalculates widthSplit. It may be updated from buildPass
  // or buildFramebuf.
  void updateWidthSplit(const VkExtent2D& extent) {
    int widthBiggest = ((extent.width > extent.height) ? 1 : 0);
    int xySwapped = cpool.vk.dev.swapChainInfo.preTransform &
                    (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR |
                     VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    widthSplit = widthBiggest ^ (xySwapped ? 1 : 0);
  }

  // Data import/conversion: load scene, copy data into vertices and indices.
  int importMesh() {
    const aiScene* scene;
    AssimpGlue assimp;
    if (assimp.import("treasure_smooth.assbin", &scene)) {
      return 1;
    }
    for (unsigned i = 0; i < scene->mNumMeshes; i++) {
      const aiMesh* mesh = scene->mMeshes[i];
      aiColor3D c(0.f, 0.f, 0.f);
      scene->mMaterials[mesh->mMaterialIndex]->Get(AI_MATKEY_COLOR_DIFFUSE, c);
      // Skip scene->mMaterials[]->GetTextureCount(aiTextureType_DIFFUSE) and
      // GetTexture(aiTextureType_DIFFUSE, 0 .. texture_count, ...) for now.

      uint32_t offset = static_cast<uint32_t>(vertices.size());
      for (unsigned i = 0; i < mesh->mNumVertices; i++) {
        aiVector3D* meshVertex = &(mesh->mVertices[i]);
        aiVector3D* meshNormal = &(mesh->mNormals[i]);
        // convert aiVector3D from OpenGL to Vulkan by flipping y.
        vertices.emplace_back(st_06threepipelines_vert{
            {meshVertex->x, -meshVertex->y, meshVertex->z},  // inPosition
            {meshNormal->x, -meshNormal->y, meshNormal->z},  // inNormal
            {c.r, c.g, c.b},                                 // inColor
        });
      }

      for (unsigned i = 0; i < mesh->mNumFaces; i++) {
        const aiFace& face = mesh->mFaces[i];
        if (face.mNumIndices != 3) {
          continue;
        }
        uglue.indices.push_back(offset + face.mIndices[0]);
        uglue.indices.push_back(offset + face.mIndices[1]);
        uglue.indices.push_back(offset + face.mIndices[2]);
      }
    }
    return 0;
  }

  int addPipe(vector<science::PipeBuilder>& pipes,
              shared_ptr<command::Shader> vertexShader, const uint32_t* data,
              size_t size) {
    auto fragmentShader = make_shared<command::Shader>(cpool.vk.dev);
    if (fragmentShader->loadSPV(data, size)) {
      return 1;
    }
    pipes.emplace_back(pass);
    return uglue.shaders.add(pipes.back(), vertexShader) ||
           uglue.shaders.add(pipes.back(), fragmentShader) ||
           // This adds the vertex input, depth format, and descriptor layout:
           uglue.initPipeBuilderFrom(pipes.back(), vertices);
  }

  int buildPass() {
    updateWidthSplit(cpool.vk.dev.swapChainInfo.imageExtent);
    vector<science::PipeBuilder> pipes;
    auto vertexShader = std::make_shared<command::Shader>(cpool.vk.dev);
    if (importMesh() ||
        vertexShader->loadSPV(spv_06threepipelines_vert,
                              sizeof(spv_06threepipelines_vert)) ||
        addPipe(pipes, vertexShader, spv_06toon_frag,
                sizeof(spv_06toon_frag)) ||
        addPipe(pipes, vertexShader, spv_06phong_frag,
                sizeof(spv_06phong_frag)) ||
        addPipe(pipes, vertexShader, spv_06wireframe_frag,
                sizeof(spv_06wireframe_frag))) {
      return 1;
    }

    if (cpool.vk.dev.enabledFeatures.features.fillModeNonSolid) {
      pipes.at(2).info().rastersci.polygonMode = VK_POLYGON_MODE_LINE;
    }
    if (cpool.vk.dev.enabledFeatures.features.wideLines) {
      pipes.at(2).info().dynamicStates.emplace_back(
          VK_DYNAMIC_STATE_LINE_WIDTH);
    }
    return uglue.buildPassAndTriggerResize();
  }

  int buildFramebuf(language::Framebuf& framebuf, size_t framebuf_i) {
    auto& cmdBuffer = uglue.cmdBuffers.at(framebuf_i);
    if (cmdBuffer.beginSimultaneousUse()) {
      logE("buildFramebuf(%zu): beginSimultaneousUse failed\n", framebuf_i);
      return 1;
    }

    VkBuffer vertexBuffers[] = {uglue.vertexBuffer.vk};
    VkDeviceSize offsets[] = {0};
    auto& newSize = cpool.vk.dev.swapChainInfo.imageExtent;

#ifndef __ANDROID__
    // Re-split the viewports along the longest axis. An Android device most
    // likely did not actually change size, only rotated...so do not recalculate
    // widthSplit for Android. This also avoids a bug where Android swaps
    // dimensions for 90 and 270 rotation, and may or may not swap at 180.
    updateWidthSplit(newSize);
#endif /* not __ANDROID__ */
    auto& enabledFeatures = cpool.vk.dev.enabledFeatures.features;
    float viewWidth = (float)newSize.width;
    float viewHeight = (float)newSize.height;
    // Divide the viewport along the long axis of the device.
    if (widthSplit) {
      viewWidth /= enabledFeatures.fillModeNonSolid ? 3 : 2;
    } else {
      viewHeight /= enabledFeatures.fillModeNonSolid ? 3 : 2;
    }
    float offsetX = 0, offsetY = 0;
    for (size_t i = 0; i < NUM_PIPELINES; i++) {
      if (cmdBuffer.beginSubpass(pass, framebuf, (uint32_t)i)) {
        logE("buildFramebuf(%zu): beginSubpass(%zu) failed\n", framebuf_i, i);
        return 1;
      }
      if (i == 2 && !enabledFeatures.fillModeNonSolid) {
        // Device does not support wireframe.
        // End the subpass immediately.
        continue;
      }

      VkViewport& view = pass.pipelines.at(i)->info.viewports.at(0);
      VkRect2D& scis = pass.pipelines.at(i)->info.scissors.at(0);
      view = {offsetX, offsetY, viewWidth, viewHeight, 0.0, 1.0};
      scis.offset = {int(offsetX), int(offsetY)};
      scis.extent = {uint32_t(viewWidth), uint32_t(viewHeight)};
      if (newSize.width > newSize.height) {
        offsetX += viewWidth;
      } else {
        offsetY += viewHeight;
      }

      if (cmdBuffer.bindGraphicsPipelineAndDescriptors(
              *pass.pipelines.at(i), 0, 1,
              &uglue.descriptorSet.at(framebuf_i)->vk) ||
          cmdBuffer.setViewport(0, 1, &view) ||
          cmdBuffer.setScissor(0, 1, &scis) ||
          // call setLineWidth only if enabled and only for pipeline 2.
          (i == 2 && enabledFeatures.wideLines &&
           cmdBuffer.setLineWidth(2.0f)) ||

          cmdBuffer.bindVertexBuffers(
              0, sizeof(vertexBuffers) / sizeof(vertexBuffers[0]),
              vertexBuffers, offsets) ||
          cmdBuffer.bindAndDraw(uglue.indices, uglue.indexBuffer.vk,
                                0 /*indexBufOffset*/)) {
        logE("buildFramebuf(%zu): bindAndDraw failed\n", framebuf_i);
        return 1;
      }
    }

    return uglue.endRenderPass(cmdBuffer, framebuf_i);
  }

  int redraw(std::shared_ptr<memory::Flight>& flight) {
    // flight->mmap() points to the uniform buffer. Write directly to it.
    auto& ubo = *reinterpret_cast<UniformBufferObject*>(flight->mmap());
    auto view = glm::lookAt(glm::vec3(-20.0f, 0.0f, 0.0f),  // Object pose.
                            glm::vec3(0.0f, 0.0f, 0.0f),    // Camera pose.
                            glm::vec3(0.0f, 1.0f, 0.0f));   // Up vector.

    ubo.lightPos = glm::vec4(0, 2, 1, 0);
    ubo.modelview = view * glm::mat4_cast(modelOrient);

    ubo.proj = glm::perspective(glm::radians(45.0f), cpool.vk.dev.aspectRatio(),
                                0.1f, 100.0f);
    ubo.proj[1][1] *= -1;  // Convert from OpenGL to Vulkan by flipping Y.

    command::SubmitInfo info;
    if (uglue.submit(flight, info)) {
      logE("uglue.submit failed\n");
      return 1;
    }
    return 0;
  }

 public:
  int run() {
    if (cpool.ctorError() || buildPass()) {
      return 1;
    }

    // Begin main loop.
    unsigned lastPrintedFrameCount = 0;
    while (!uglue.windowShouldClose()) {
      if (uglue.elapsed.get() > 1.0) {
        logI("%d fps\n", uglue.frameNumber - lastPrintedFrameCount);
        uglue.elapsed.reset();
        lastPrintedFrameCount = uglue.frameNumber;
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
      // Request features here, verify success above.
      inst.devs.at(0)->enabledFeatures.set("fillModeNonSolid", VK_TRUE) ||
      inst.devs.at(0)->enabledFeatures.set("wideLines", VK_TRUE) ||
      // inst.open() takes a while, especially if validation layers are on.
      inst.open({(uint32_t)width, (uint32_t)height})) {
    logE("inst.ctorError or enabledFeatures or inst.open failed\n");
  } else if (!inst.devs.size()) {
    logE("No vulkan devices found (or driver missing?)\n");
  } else {
    r = std::make_shared<Example06>(inst, window)->run();
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
  GLFWwindow* window = glfwCreateWindow(
      800, 600, "06threepipelines Vulkan window",
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
