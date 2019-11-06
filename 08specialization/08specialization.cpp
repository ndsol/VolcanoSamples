/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This sample demonstrates specialization constants.
 */

#include "../src/assimpglue.h"
#include "../src/load_gli.h"
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

#include "08specialization/08specialization.frag.h"
#include "08specialization/08specialization.vert.h"
#include "imgui.h"

using namespace std;
namespace example {

#include "08specialization/struct_08specialization.vert.h"
namespace frag {
#include "08specialization/struct_08specialization.frag.h"
}

// Example08 is documented at github.com/ndsol/VolcanoSamples/08specialization/
class Example08 : public BaseApplication {
 public:
  Example08(language::Instance& instance, GLFWwindow* window)
      : BaseApplication{instance},
        uglue{*this, window, 0 /*maxLayoutIndex*/,
              bindingIndexOfUniformBufferObject(),
              sizeof(UniformBufferObject)} {
    resizeFramebufListeners.emplace_back(std::make_pair(
        [](void* self, language::Framebuf& framebuf, size_t fbi,
           size_t) -> int {
          return static_cast<Example08*>(self)->buildFramebuf(framebuf, fbi);
        },
        this));
    uglue.redrawListeners.emplace_back(std::make_pair(
        [](void* self, std::shared_ptr<memory::Flight>& flight) -> int {
          return static_cast<Example08*>(self)->redraw(flight);
        },
        this));
    uglue.keyEventListeners.push_back(std::make_pair(
        [](void* self, int key, int /*scancode*/, int action,
           int /*mods*/) -> void {
          if (action != GLFW_PRESS && action != GLFW_REPEAT) {
            return;
          }
          static_cast<Example08*>(self)->onModelRotate(
              key == GLFW_KEY_LEFT ? -10 : (key == GLFW_KEY_RIGHT ? 10 : 0),
              key == GLFW_KEY_UP ? -10 : (key == GLFW_KEY_DOWN ? 10 : 0));
        },
        this));
    uglue.inputEventListeners.push_back(std::make_pair(
        [](void* self, GLFWinputEvent* e, size_t eCount, int m,
           int enter) -> void {
          static_cast<Example08*>(self)->onInputEvent(e, eCount, m, enter);
        },
        this));
  }

 protected:
  UniformGlue uglue;
  // modelOrient represents the model matrix as a single rotation quaternion.
  glm::quat modelOrient =
      glm::angleAxis((float)M_PI * -.5f, glm::vec3(0, 0, 1));
  // widthSplit is the calculated width for each of the 3 viewports.
  int widthSplit{0};
  // vertices is the vertex buffer until initUniformAndPipeBuilder copies it to
  // the device. The vector<uint32_t> indices (the index buffer) is
  // conveniently provided by uglue.
  vector<st_08specialization_vert> vertices;
  science::Sampler zebra{cpool.vk.dev};
  static constexpr int NUM_PIPELINES = 4;
  static constexpr float deadzone = 0.1;

  void onModelRotate(float dx, float dy) {
    if (!dx && !dy) {
      return;
    }
    static constexpr float perPx = 1.0f / 8;  // Rotation per pixel.
    auto rx = glm::angleAxis(glm::radians(dx * perPx), glm::vec3(0, 1, 0));
    auto ry = glm::angleAxis(glm::radians(dy * perPx), glm::vec3(0, 0, 1));
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

  // updateWidthSplit recalculates widthSplit. It may be updated from buildPass
  // or buildFramebuf.
  void updateWidthSplit(const VkExtent2D& extent) {
    int widthBiggest = ((extent.width > extent.height) ? 1 : 0);
    int xySwapped = cpool.vk.dev.swapChainInfo.preTransform &
                    (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR |
                     VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    widthSplit = widthBiggest ^ (xySwapped ? 1 : 0);
  }

  aiVector3D convertOrMakeUV(const aiMesh* mesh, unsigned vtxIndex) {
    aiVector3D* textureCoords = mesh->mTextureCoords[0];
    if (!textureCoords) {
      return aiVector3D(0.2 * ((vtxIndex & 3) > 1),
                        0.2 * (((vtxIndex + 1) & 3) < 2), 0);
    }
    return textureCoords[vtxIndex];
  }

  // Data import/conversion: load scene, copy data into vertices and indices.
  int importMesh() {
    const aiScene* scene;
    AssimpGlue assimp;
    if (assimp.import("08model.assbin", &scene)) {
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
        aiVector3D meshUV = convertOrMakeUV(mesh, i);
        // convert aiVector3D from OpenGL to Vulkan by flipping y.
        vertices.emplace_back(st_08specialization_vert{
            {meshVertex->x, -meshVertex->y, meshVertex->z},  // inPosition
            {meshNormal->x, -meshNormal->y, meshNormal->z},  // inNormal
            {c.r, c.g, c.b},                                 // inColor
            {meshUV.x, meshUV.y},                            // inTexCoord
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

  int initZebra() {
    std::string textureFound;
    gli::texture gli_mem;
    // loadGLI is defined in src/load_gli.cpp
    if (loadGLI("08texture.ktx", textureFound, gli_mem)) {
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

    zebra.info.anisotropyEnable = VK_TRUE;
    zebra.info.magFilter = VK_FILTER_LINEAR;
    zebra.info.minFilter = VK_FILTER_LINEAR;
    zebra.info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    zebra.info.maxLod = tex.levels();
    // constructSamplerGLI is defined in src/load_gli.cpp
    return zebra.setName("zebra") ||
           zebra.imageView.setName("zebra.imageView") ||
           zebra.image->setName("zebra.image") ||
           constructSamplerGLI(zebra, uglue.stage, tex);
  }

  int buildPass() {
    updateWidthSplit(cpool.vk.dev.swapChainInfo.imageExtent);

    vector<science::PipeBuilder> pipes;
    auto vertexShader = std::make_shared<command::Shader>(cpool.vk.dev);
    auto fragmentShader = make_shared<command::Shader>(cpool.vk.dev);
    if (importMesh() ||
        vertexShader->loadSPV(spv_08specialization_vert,
                              sizeof(spv_08specialization_vert)) ||
        fragmentShader->loadSPV(spv_08specialization_frag,
                                sizeof(spv_08specialization_frag)) ||
        initZebra()) {
      logE("mesh or shader or texture load failed\n");
      return 1;
    }
    if (cpool.setName("cpool") || pass.setName("pass") ||
        cpool.vk.dev.setName("cpool.vk.dev") ||
        cpool.vk.dev.setSurfaceName("inst.surface") ||
        cpool.vk.dev.swapChain.setName("cpool.vk.dev.swapChain") ||
        vertexShader->setName("vertexShader") ||
        fragmentShader->setName("fragmentShader") ||
        uglue.indexBuffer.setName("uglue.indexBuffer") ||
        uglue.vertexBuffer.setName("uglue.vertexBuffer") ||
        uglue.renderSemaphore.setName("uglue.renderSemaphore") ||
        uglue.imageAvailableSemaphore.setName("imageAvailableSemaphore") ||
        uglue.renderDoneFence.setName("uglue.renderDoneFence")) {
      logE("buildPass: cpool or pass or uglue.*.setName failed\n");
      return 1;
    }
    for (int i = 0; i < NUM_PIPELINES; i++) {
      // Create a PipeBuilder for this fragmentShader.
      pipes.emplace_back(pass);
      auto& thePipe = pipes.back();
      if (uglue.shaders.add(thePipe, vertexShader) ||
          uglue.shaders.add(thePipe, fragmentShader) ||
          uglue.initPipeBuilderFrom(thePipe, vertices)) {
        logE("pipe[%d] failed\n", i);
        return 1;
      }

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
        pipes.at(2).pipe->pipelineLayout.setName("pipe[2] layout") ||
        pipes.at(3).setName("pipe[3] textured") ||
        pipes.at(3).pipe->pipelineLayout.setName("pipe[3] layout")) {
      logE("pipe[*] setName failed\n");
      return 1;
    }
    return uglue.buildPassAndTriggerResize();
  }

  int buildFramebuf(language::Framebuf& framebuf, size_t framebuf_i) {
    char name[256];
    snprintf(name, sizeof(name), "uglue.descriptorSet[%zu]", framebuf_i);
    if (uglue.descriptorSet.at(framebuf_i)->setName(name) ||
        uglue.descriptorSet.at(framebuf_i)
            ->write(frag::bindingIndexOftexSampler(),
                    std::vector<science::Sampler*>{&zebra})) {
      logE("uglue.descriptorSet[%zu] failed\n", framebuf_i);
      return 1;
    }

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

    VkBuffer vertexBuffers[] = {uglue.vertexBuffer.vk};
    VkDeviceSize offsets[] = {0};
    auto& newSize = cpool.vk.dev.swapChainInfo.imageExtent;

#ifndef __ANDROID__
    updateWidthSplit(newSize);
#endif /* not __ANDROID__ */
    auto& enabledFeatures = cpool.vk.dev.enabledFeatures.features;
    float viewWidth = (float)newSize.width;
    float viewHeight = (float)newSize.height;
    // Divide the viewport along the long axis of the device.
    if (widthSplit) {
      viewWidth /= NUM_PIPELINES - (enabledFeatures.fillModeNonSolid ? 0 : 1);
    } else {
      viewHeight /= NUM_PIPELINES - (enabledFeatures.fillModeNonSolid ? 0 : 1);
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
    onModelRotate(uglue.curJoyX, uglue.curJoyY);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(64, 64));
    static constexpr int NonWindow = ImGuiWindowFlags_NoTitleBar |
                                     ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoInputs;
    ImGui::Begin("FPS", NULL, NonWindow);
    ImGui::Text("%.0ffps", ImGui::GetIO().Framerate);
    ImGui::End();

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
    if (cpool.ctorError() || uglue.imGuiInit() || buildPass() ||
        uglue.descriptorLibrary.setName("uglue.descriptorLibrary")) {
      logE("cpool or buildPass failed\n");
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
    r = std::make_shared<Example08>(inst, window)->run();
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
      800, 600, "08specialization Vulkan window",
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
