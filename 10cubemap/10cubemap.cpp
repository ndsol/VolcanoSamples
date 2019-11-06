/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This sample demonstrates a cubemap.
 */
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "../src/assimpglue.h"
#include "../src/scanlinedecoder.h"
#include "../src/uniformglue/uniformglue.h"

// uniform_glue.h has already #included some glm headers.
#include <assimp/scene.h>

#include <algorithm>

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

#include "10cubemap/10cubemap.frag.h"
#include "10cubemap/10cubemap.vert.h"
#include "imgui.h"

#ifdef __ANDROID__
#include <android/keycodes.h>
#endif /*__ANDROID__*/

using namespace std;
namespace example {

#include "10cubemap/struct_10cubemap.vert.h"
namespace frag {
#include "10cubemap/struct_10cubemap.frag.h"
}

// Example10 is documented at github.com/ndsol/VolcanoSamples/10cubemap/
class Example10 : public BaseApplication {
 public:
  Example10(language::Instance& instance, GLFWwindow* window)
      : BaseApplication{instance},
        uglue{*this, window, 0 /*maxLayoutIndex*/,
              bindingIndexOfUniformBufferObject(),
              sizeof(UniformBufferObject)} {
    resizeFramebufListeners.emplace_back(std::make_pair(
        [](void* self, language::Framebuf& framebuf, size_t fbi,
           size_t) -> int {
          return static_cast<Example10*>(self)->buildFramebuf(framebuf, fbi);
        },
        this));
    uglue.redrawListeners.emplace_back(std::make_pair(
        [](void* self, std::shared_ptr<memory::Flight>& flight) -> int {
          return static_cast<Example10*>(self)->redraw(flight);
        },
        this));
    uglue.keyEventListeners.push_back(std::make_pair(
        [](void* self, int key, int /*scancode*/, int action,
           int /*mods*/) -> void {
          if (action != GLFW_PRESS && action != GLFW_REPEAT) {
            return;
          }
          static_cast<Example10*>(self)->onModelRotate(
              key == GLFW_KEY_LEFT ? -10 : (key == GLFW_KEY_RIGHT ? 10 : 0),
              key == GLFW_KEY_UP ? -10 : (key == GLFW_KEY_DOWN ? 10 : 0));
        },
        this));
    uglue.inputEventListeners.push_back(std::make_pair(
        [](void* self, GLFWinputEvent* e, size_t eCount, int m,
           int enter) -> void {
          static_cast<Example10*>(self)->onInputEvent(e, eCount, m, enter);
        },
        this));
  }

 protected:
  struct Slider {
    const char* name;
    const size_t offset;
    float value;
  };
  std::vector<Slider> sliders{
      Slider{"grass", offsetof(UniformBufferObject, grassReflect), 0.05f},
      Slider{"tree", offsetof(UniformBufferObject, treeReflect), 0.05f},
      Slider{"rock", offsetof(UniformBufferObject, rockReflect), 0.1f},
      Slider{"dirt", offsetof(UniformBufferObject, dirtReflect), 0.0f},
  };
  UniformGlue uglue;
  science::PipeBuilder pipe0{pass};
  ScanlineDecoder codec{uglue.stage};
  vector<st_10cubemap_vert> vertices;
  glm::quat modelOrient =
      glm::angleAxis((float)M_PI * -.5f, glm::vec3(0, 0, 1));

  void onModelRotate(float dx, float dy) {
    static constexpr float perPx = 1.0f / 1;  // Rotation per pixel.
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
    onModelRotate(dx, dy);
  }

  int addSkyboxToVertexIndex() {
    // Tell vertex shader it is the skybox by setting normal to (0, 0, 0).
    auto zero = glm::vec3(0, 0, 0);

    // sv array contains the vertices for the skybox:
    st_10cubemap_vert sv[] = {
        glm::vec3(-1., -1., -1.), zero, zero, glm::vec2(0, 0),  // UV does not
        glm::vec3(1., -1., -1.),  zero, zero, glm::vec2(0, 0),  // get used
        glm::vec3(-1., 1., -1.),  zero, zero, glm::vec2(0, 0),  // by skybox
        glm::vec3(1., 1., -1.),   zero, zero, glm::vec2(0, 0),  // texture()
        glm::vec3(-1., -1., 1.),  zero, zero, glm::vec2(0, 0),  // call.
        glm::vec3(1., -1., 1.),   zero, zero, glm::vec2(0, 0),
        glm::vec3(-1., 1., 1.),   zero, zero, glm::vec2(0, 0),
        glm::vec3(1., 1., 1.),    zero, zero, glm::vec2(0, 0),
    };
    uint32_t offset = static_cast<uint32_t>(vertices.size());
    vertices.insert(vertices.end(), &sv[0], &sv[sizeof(sv) / sizeof(sv[0])]);

    uint32_t skybox[] = {
        offset + 0, offset + 2, offset + 3,  // Back
        offset + 3, offset + 1, offset + 0,

        offset + 4, offset + 5, offset + 7,  // Front
        offset + 7, offset + 6, offset + 4,

        offset + 0, offset + 4, offset + 6,  // Left
        offset + 6, offset + 2, offset + 0,

        offset + 1, offset + 3, offset + 7,  // Right
        offset + 7, offset + 5, offset + 1,

        offset + 0, offset + 1, offset + 5,  // Top
        offset + 5, offset + 4, offset + 0,

        offset + 2, offset + 6, offset + 7,  // Bottom
        offset + 7, offset + 3, offset + 2,
    };
    uglue.indices.insert(uglue.indices.end(), &skybox[0],
                         &skybox[sizeof(skybox) / sizeof(skybox[0])]);
    return 0;
  }

  aiVector3D convertOrMakeUV(const aiMesh* mesh, unsigned vtxIndex) {
    aiVector3D* textureCoords = mesh->mTextureCoords[0];
    if (!textureCoords) {
      return aiVector3D((vtxIndex & 3) > 1, ((vtxIndex + 1) & 3) < 2, 0);
    }
    return textureCoords[vtxIndex];
  }

  // Data import/conversion: load scene, copy data into vertices and indices.
  int importMesh() {
    if (addSkyboxToVertexIndex()) {
      logE("addSkyboxToVertexIndex failed\n");
      return 1;
    }
    // Load scene from .assbin file
    const aiScene* scene;
    AssimpGlue assimp;
    if (assimp.import("10model.assbin", &scene)) {
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
        vertices.emplace_back(st_10cubemap_vert{
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

  int loadTextures() {
    VkExtent3D extent;
    static constexpr int CUBE_FACES = 6;
#ifdef __ANDROID__
    bool enableMipmaps = cpool.vk.dev.physProp.properties.vendorID != 0x5143;
    if (!enableMipmaps) {
      // Semi-related: https://github.com/mrdoob/three.js/issues/9988
      logW("Qualcomm Adreno cubemap-mipmaps disabled.\n");
      logW("Issue is dFdx/dFdy invalid for anything level 1+ at seams.\n");
    }
#else  /*__ANDROID__*/
    static constexpr bool enableMipmaps = true;
#endif /*__ANDROID__*/

    // Read input files.
    for (int i = 0; i < CUBE_FACES; i++) {
      char filename[256];
      snprintf(filename, sizeof(filename), "cubemap%d.jpg", i);
      if (codec.open(filename)) {
        return 1;
      }
      if (!enableMipmaps) {
        codec.sampler.image->info.mipLevels = 1;
      }

      if (i == 0) {
        extent = codec.sampler.image->info.extent;

        // Populate codec.sampler.info for VkSampler
        codec.sampler.info.anisotropyEnable = VK_TRUE;
        codec.sampler.info.magFilter = VK_FILTER_LINEAR;
        codec.sampler.info.minFilter = VK_FILTER_LINEAR;
        codec.sampler.info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        // CLAMP_TO_EDGE prevents artifacts from appearing at the edges.
        codec.sampler.info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        codec.sampler.info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        codec.sampler.info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        // Populate codec.sampler.image->info for VkImage
        codec.sampler.image->info.arrayLayers = CUBE_FACES;
        codec.sampler.image->info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        codec.sampler.image->info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_SAMPLED_BIT;

        // Populate codec.sampler.imageView.info for VkImageView
        codec.sampler.imageView.info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        codec.sampler.imageView.info.subresourceRange =
            codec.sampler.image->getSubresourceRange();
        codec.sampler.imageView.info.subresourceRange.layerCount = CUBE_FACES;

        if (codec.sampler.ctorError()) {
          logE("codec.sampler.ctorError failed\n");
          return 1;
        }
      } else {
        VkExtent3D& got = codec.sampler.image->info.extent;
        if (got.width != extent.width || got.height != extent.height ||
            got.depth != extent.depth) {
          logE("%s != cubemap0.jpg: got %u x %u x %u, want %u x %u x %u\n",
               filename, got.width, got.height, got.depth, extent.width,
               extent.height, extent.depth);
          return 1;
        }
        codec.dstArrayLayer = i;
      }

      while (codec.moreLines()) {
        if (codec.read()) {
          logE("read(%s) failed at line %zu\n", filename,
               (size_t)codec.cpu.lineCount);
          return 1;
        }
      }
    }
    // Transition the image layout of codec.sampler.
    science::SmartCommandBuffer smart(uglue.stage.pool, uglue.stage.poolQindex);
    if (smart.ctorError() || smart.autoSubmit() ||
        smart.barrier(*codec.sampler.image,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
      logE("barrier(SHADER_READ_ONLY) failed\n");
      return 1;
    }
    return 0;
  }

  int buildPass() {
    // If no alpha blending is used, change from VK_COMPARE_OP_LESS to
    // VK_COMPARE_OP_LESS_OR_EQUAL because the skybox is draw at Z = 1.0,
    // equal to the empty (cleared) depth buffer.
    pipe0.info().depthsci.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    pipe0.info().rastersci.cullMode = VK_CULL_MODE_NONE;
    pipe0.info().perFramebufColorBlend.at(0) =
        command::PipelineCreateInfo::withEnabledAlpha();
    auto vert = std::make_shared<command::Shader>(cpool.vk.dev);
    auto frag = make_shared<command::Shader>(cpool.vk.dev);
    return importMesh() ||
           vert->loadSPV(spv_10cubemap_vert, sizeof(spv_10cubemap_vert)) ||
           frag->loadSPV(spv_10cubemap_frag, sizeof(spv_10cubemap_frag)) ||
           loadTextures() || uglue.shaders.add(pipe0, vert) ||
           uglue.shaders.add(pipe0, frag) ||
           uglue.initPipeBuilderFrom(pipe0, vertices) ||
           cpool.setName("cpool") || pass.setName("pass") ||
           vert->setName("vert") || frag->setName("frag") ||
           cpool.vk.dev.setName("cpool.vk.dev") ||
           cpool.vk.dev.setSurfaceName("inst.surface") ||
           cpool.vk.dev.swapChain.setName("cpool.vk.dev.swapChain") ||
           uglue.indexBuffer.setName("uglue.indexBuffer") ||
           uglue.vertexBuffer.setName("uglue.vertexBuffer") ||
           pipe0.setName("pipe0") ||
           pipe0.pipe->pipelineLayout.setName("pipe0 layout") ||
           codec.sampler.setName("codec.sampler") ||
           codec.sampler.image->setName("codec.sampler.image") ||
           codec.sampler.imageView.setName("codec.sampler.imageView") ||
           uglue.renderSemaphore.setName("uglue.renderSemaphore") ||
           uglue.imageAvailableSemaphore.setName("imageAvailableSemaphore") ||
           uglue.renderDoneFence.setName("uglue.renderDoneFence") ||
           uglue.buildPassAndTriggerResize();
  }

  int buildFramebuf(language::Framebuf& framebuf, size_t framebuf_i) {
    char name[256];
    snprintf(name, sizeof(name), "uglue.descriptorSet[%zu]", framebuf_i);
    if (uglue.descriptorSet.at(framebuf_i)->setName(name) ||
        uglue.descriptorSet.at(framebuf_i)
            ->write(frag::bindingIndexOfcubemap(),
                    std::vector<science::Sampler*>{&codec.sampler})) {
      return 1;
    }

    snprintf(name, sizeof(name), "uglue.uniform[%zu]", framebuf_i);
    if (uglue.uniform.at(framebuf_i).setName(name)) {
      logE("uglue.uniform[%zu].setName failed\n", framebuf_i);
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

  int redraw(std::shared_ptr<memory::Flight>& flight) {
    onModelRotate(uglue.curJoyX, uglue.curJoyY);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetFontSize() * 10, 64),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 20, 0),
                             ImGuiCond_FirstUseEver);
    ImGui::Begin("Config");
    ImGui::Text("%.0ffps", ImGui::GetIO().Framerate);
    float sliderWidth = ImGui::GetWindowWidth() - ImGui::GetFontSize();
    for (auto it = sliders.begin(); it != sliders.end(); it++) {
      ImGui::Text("<transparent %s | shiny %s>", it->name, it->name);
      ImGui::PushItemWidth(sliderWidth);
      ImGui::SliderFloat(it->name, &it->value, -1.f, 1.f, "%.2f");
      ImGui::PopItemWidth();
    }
    ImGui::End();

    auto& ubo = *reinterpret_cast<UniformBufferObject*>(flight->mmap());
    auto view = glm::lookAt(glm::vec3(-20.0f, 0.0f, 0.0f),  // Object pose.
                            glm::vec3(0.0f, 0.0f, 0.0f),    // Camera pose.
                            glm::vec3(0.0f, 1.0f, 0.0f));   // Up vector.

    ubo.lightPos = glm::vec4(0, 2, 1, 0);
    ubo.modelview = view * glm::mat4_cast(modelOrient);
    ubo.invModelView = glm::inverse(ubo.modelview);

    ubo.proj = glm::perspective(glm::radians(45.0f), cpool.vk.dev.aspectRatio(),
                                0.1f, 100.0f);
    ubo.proj[1][1] *= -1;  // Convert from OpenGL to Vulkan by flipping Y.
    for (auto it = sliders.begin(); it != sliders.end(); it++) {
      *(float*)((char*)&ubo + it->offset) = it->value;
    }

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
      // inst.open() takes a while, especially if validation layers are on.
      inst.open({(uint32_t)width, (uint32_t)height})) {
    logE("inst.ctorError or inst.open failed\n");
  } else if (!inst.devs.size()) {
    logE("No vulkan devices found (or driver missing?)\n");
  } else {
    r = std::make_shared<Example10>(inst, window)->run();
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
  GLFWwindow* window = glfwCreateWindow(800, 600, "10cubemap Vulkan window",
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
