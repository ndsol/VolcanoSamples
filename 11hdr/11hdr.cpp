/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This sample demonstrates high dynamic range concepts.
 */

#include "../src/assimpglue.h"
#include "../src/scanlinedecoder.h"
#include "../src/uniformglue/uniformglue.h"

// uniform_glue.h has already #included some glm headers.
#include <assimp/scene.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <gli/gli.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#define _USE_MATH_DEFINES /*Windows otherwise hides M_PI*/
#include <math.h>

#include "11hdr/11hdr.frag.h"
#include "11hdr/11hdr.vert.h"
#include "11hdr/11sphere2cube.frag.h"
#include "11hdr/11sphere2cube.vert.h"
#include "imgui.h"

#ifdef __ANDROID__
#include <android/keycodes.h>
#endif /*__ANDROID__*/

using namespace std;
namespace example {

namespace vert {
#include "11hdr/struct_11hdr.vert.h"
}
namespace frag {
#include "11hdr/struct_11hdr.frag.h"
}
namespace sphere2cubefrag {
#include "11hdr/struct_11sphere2cube.frag.h"
}

class ImGuiLogOutputOverride {
 public:
  ImGuiLogOutputOverride(UniformGlue& uglue, VolcanoLogFn prevLog) {
    prevLogger = prevLog;
    captureLogs();

    logFontConfig = std::make_shared<ImFontConfig>();
    logFontConfig->SizePixels = 13.f;
    uglue.fonts.push_back(logFontConfig);
    logFontPos = uglue.fonts.size() - 1;
  }

  // The constructor calls captureLogs, but it is also ok to call captureLogs
  // very early in the app startup. Just be sure to save the previous value of
  // logVolcano and pass it to the constructor.
  static void captureLogs() {
    logVolcano = iglooLogger;
    gSkDebugVPrinter = skDebugVPrinter;
  }

  virtual ~ImGuiLogOutputOverride() {
    // Dump all logs. If the app does not need to dump logs, i.e. when the app
    // is exiting normally, call igloo.clear() first so this doesn't print
    // anything.
    logVolcano = prevLogger;
    if (!text.empty() || !logBuffer.empty()) {
      logI("igloo log:\n%.*s%.*s", (int)text.size(), text.data(),
           (int)logBuffer.size(), logBuffer.data());
    }
  }

  void clear() { text.clear(); }

  int ctorError() {
    auto& io = ImGui::GetIO();
    if (io.Fonts->Fonts.Size <= (int)logFontPos) {
      logE("Failed to add logFont\n");
      return 1;
    }
    logFont = io.Fonts->Fonts[logFontPos];
    return 0;
  }

  int render() {
    if (!logBuffer.empty()) {
      auto p = text.end();
      if (!text.empty()) {
        p--;  // Do not duplicate the '\0' terminator.
        text.insert(p, logBuffer.begin(), logBuffer.end() - 1);
      } else {
        text.insert(p, logBuffer.begin(), logBuffer.end());
      }
      logBuffer.clear();
      scrollToBottom = true;
    }

    if (ImGui::Begin("Log")) {
      ImGui::PushFont(logFont);
      if (text.size()) {
        ImGui::TextUnformatted(text.data(), text.data() + text.size() - 1);
      }
      if (scrollToBottom &&
          ImGui::GetScrollY() >
              ImGui::GetScrollMaxY() - logFontConfig->SizePixels) {
        ImGui::SetScrollHereY(1.f);
        scrollToBottom = false;
      }
      ImGui::PopFont();
    }
    ImGui::End();
    return 0;
  }

 protected:
  // logBuffer is a static object to collect logs until render() is called.
  static std::vector<char> logBuffer;

  // iglooLogger overrides the default logger and collects the output in
  // logBuffer instead.
  static void iglooLogger(char level, const char* fmt, va_list ap) {
    // Use ap to ask vsnprintf how many char to allocate.
    va_list ap2;
    va_copy(ap2, ap);  // va_copy() before first use of ap.
    int needBytes = vsnprintf(NULL, 0, fmt, ap);
    if (needBytes <= 0) {
      // Failed to get the size from vsnprintf. Silently fail.
      return;
    }
    needBytes++;  // Count '\0' terminator.

    // Grow logBuffer.
    size_t len = logBuffer.size();
    if (len > 0) {
      len--;  // Do not create a duplicate '\0' terminator.
    }
    logBuffer.resize(len + needBytes + 2);
    logBuffer.at(len) = level;
    logBuffer.at(len + 1) = ' ';

    vsnprintf(logBuffer.data() + len + 2, needBytes, fmt, ap2);
    va_end(ap2);

    if (level == 'F') {
      fputs(logBuffer.data(), stderr);
      exit(1);
    }
  }

  // skDebugVPrinter forwards skia log messages to iglooLogger.
  static SK_API void skDebugVPrinter(const char format[], va_list ap) {
    logVolcano('E', format, ap);
  }

  // prevLogger saves the default logger and puts it back in the destructor.
  VolcanoLogFn prevLogger = nullptr;

  // text is the log text rendered to the screen.
  std::vector<char> text;
  ImFont* logFont{nullptr};
  size_t logFontPos{0};
  std::shared_ptr<ImFontConfig> logFontConfig;
  bool scrollToBottom{false};
};

std::vector<char> ImGuiLogOutputOverride::logBuffer;

// Example11 is documented at github.com/ndsol/VolcanoSamples/11hdr/
class Example11 : public BaseApplication {
 public:
  Example11(language::Instance& instance, GLFWwindow* window,
            VolcanoLogFn prevLogger)
      : BaseApplication{instance},
        uglue{*this, window, 0 /*maxLayoutIndex*/,
              vert::bindingIndexOfUniformBufferObject(),
              sizeof(vert::UniformBufferObject)},
        igloo{uglue, prevLogger} {
    resizeFramebufListeners.emplace_back(std::make_pair(
        [](void* self, language::Framebuf& framebuf, size_t fbi,
           size_t) -> int {
          return static_cast<Example11*>(self)->buildFramebuf(framebuf, fbi);
        },
        this));
    uglue.redrawListeners.emplace_back(std::make_pair(
        [](void* self, std::shared_ptr<memory::Flight>& flight) -> int {
          return static_cast<Example11*>(self)->redraw(flight);
        },
        this));
    uglue.keyEventListeners.push_back(std::make_pair(
        [](void* self, int key, int /*scancode*/, int action,
           int /*mods*/) -> void {
          if (action != GLFW_PRESS && action != GLFW_REPEAT) {
            return;
          }
          static_cast<Example11*>(self)->onModelRotate(
              key == GLFW_KEY_LEFT ? -10 : (key == GLFW_KEY_RIGHT ? 10 : 0),
              key == GLFW_KEY_UP ? -10 : (key == GLFW_KEY_DOWN ? 10 : 0));
        },
        this));
    uglue.inputEventListeners.push_back(std::make_pair(
        [](void* self, GLFWinputEvent* e, size_t eCount, int m,
           int enter) -> void {
          static_cast<Example11*>(self)->onInputEvent(e, eCount, m, enter);
        },
        this));
  }

 protected:
  UniformGlue uglue;
  ImGuiLogOutputOverride igloo;
  science::PipeBuilder pipe0{pass};
  ScanlineDecoder codec{uglue.stage};
  vector<vert::st_11hdr_vert> vertices;
  glm::quat modelOrient =
      glm::angleAxis((float)M_PI * -.5f, glm::vec3(0, 0, 1));

  int initStep{0};
  const int maxInitStep{4};
  float turn{0};
  float cubeDebug{1};

  std::shared_ptr<memory::Image> proj;
  command::RenderPass projPass1{cpool.vk.dev};
  science::PipeBuilder projPipe1{projPass1};
  command::RenderPass projPass2{cpool.vk.dev};
  science::PipeBuilder projPipe2{projPass2};
  science::ShaderLibrary projShaders{cpool.vk.dev};
  science::DescriptorLibrary projDescriptorLibrary{cpool.vk.dev};
  std::vector<std::shared_ptr<memory::DescriptorSet>> projDescriptor;
  science::Sampler cubemap{cpool.vk.dev};
  static constexpr int CUBE_FACES = 6;

  int constructCubemap(uint32_t width, uint32_t height) {
    // Populate cubemap.info for VkSampler
    cubemap.info.anisotropyEnable = VK_TRUE;
    cubemap.info.magFilter = VK_FILTER_LINEAR;
    cubemap.info.minFilter = VK_FILTER_LINEAR;
    cubemap.info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // CLAMP_TO_EDGE prevents artifacts from appearing at the edges.
    cubemap.info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    cubemap.info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    cubemap.info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    // Populate cubemap.image->info for VkImage
    cubemap.image->info.extent.width = width;
    cubemap.image->info.extent.height = height;
    cubemap.image->info.extent.depth = 1;
    cubemap.image->info.format = cpool.vk.dev.swapChainInfo.imageFormat;
    cubemap.image->info.arrayLayers = CUBE_FACES;
    cubemap.image->info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    cubemap.image->info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT;

    // Populate cubemap.imageView.info for VkImageView
    cubemap.imageView.info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    cubemap.imageView.info.subresourceRange =
        cubemap.image->getSubresourceRange();
    cubemap.imageView.info.subresourceRange.layerCount = CUBE_FACES;

    if (cubemap.image->setMipLevelsFromExtent() || cubemap.ctorError()) {
      logE("cubemap.setMipLevels or .ctorError failed\n");
      return 1;
    }
    science::SmartCommandBuffer smart(uglue.stage.pool, uglue.stage.poolQindex);
    if (smart.ctorError() || smart.autoSubmit() ||
        smart.barrier(*cubemap.image,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
      logE("barrier(SHADER_READ_ONLY) failed\n");
      return 1;
    }
    return 0;
  }

  int writeToCubemap(command::CommandBuffer& cmd, int outputNum,
                     int imgOffset) {
    VkImageCopy copy0;
    memset(&copy0, 0, sizeof(copy0));
    copy0.srcSubresource = proj->getSubresourceLayers(0 /*mip level*/);
    copy0.srcSubresource.baseArrayLayer = 0;
    copy0.srcSubresource.layerCount = 1;
    copy0.dstSubresource = cubemap.image->getSubresourceLayers(0 /*mip level*/);
    copy0.dstSubresource.baseArrayLayer = outputNum;
    copy0.dstSubresource.layerCount = 1;

    // Use a custom srcOffset, extent to read just a part of cubemap.
    copy0.extent = cubemap.image->info.extent;
    copy0.srcOffset = {int32_t(copy0.extent.width) * imgOffset, 0, 0};

    if (cmd.barrier(*cubemap.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) ||
        cmd.barrier(*proj, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) ||
        cmd.copyImage(*proj, *cubemap.image, {copy0}) ||
        science::copyImageToMipmap(cmd, *cubemap.image)) {
      logE("writeToCubemap: cmd failed\n");
      return 1;
    }
    return 0;
  }

  int renderProj(command::CommandBuffer& cmd, command::RenderPass& pass,
                 science::PipeBuilder& pb, memory::DescriptorSet& ds,
                 size_t framebuf_i) {
    VkDescriptorBufferInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.buffer = uglue.uniform.at(framebuf_i).vk;
    bi.range = uglue.uniform.at(framebuf_i).info.size;
    if (cmd.barrier(*codec.sampler.image,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
        // ds.write() uses codec.sampler.image->currentLayout, so
        // be careful to only call write() after the call to cmd.barrier().
        ds.write(sphere2cubefrag::bindingIndexOftexSampler(),
                 std::vector<science::Sampler*>{&codec.sampler}) ||
        ds.write(sphere2cubefrag::bindingIndexOfUniformBufferObject(), {bi})) {
      logE("barrier or ds.write failed\n");
      return 1;
    }
    proj->currentLayout = pb.info().attach.at(0).vk.finalLayout;
    if (cmd.beginSubpass(pass, pass.getTargetFramebuf(), 0) ||
        cmd.bindGraphicsPipelineAndDescriptors(*pb.pipe, 0, 1, &ds.vk) ||
        cmd.setViewport(0, 1, &pb.info().viewports.at(0)) ||
        cmd.setScissor(0, 1, &pb.info().scissors.at(0)) ||
        cmd.draw(4, 1, 0, 0) ||
        // End RenderPass.
        cmd.endRenderPass()) {
      logE("renderProj: command buffer failed.\n");
      return 1;
    }
    return 0;
  }

  int populateProjUBO(void* map) {
    auto* ubo = reinterpret_cast<sphere2cubefrag::UniformBufferObject*>(map);
    ubo->turn = turn;
    return 0;
  }

  // projectToCube converts an equirectangular projection (reading from
  // codec.sampler.image) to proj.
  int projectToCube(command::CommandBuffer& cmd, size_t framebuf_i) {
    if (!codec.sampler.image || !proj) {
      logE("must finish initStep first: sampler.image or proj is NULL\n");
      return 1;
    }
    if (sizeof(sphere2cubefrag::UniformBufferObject) !=
        uglue.uniform.at(framebuf_i).info.size) {
      // By having identical UBO layouts, the same buffer can be reused.
      logE("sphere2cube.frag and 11hdr.frag UniformBufferObject differ.\n");
      return 1;
    }
    if (renderProj(cmd, projPass1, projPipe1,
                   *projDescriptor.at(framebuf_i * 2), framebuf_i) ||
        writeToCubemap(cmd, 0, 2) ||  // +X face (matches OpenGL order)
        writeToCubemap(cmd, 1, 0) ||  // -X face
        writeToCubemap(cmd, 4, 1) ||  // +Z face
        writeToCubemap(cmd, 5, 3)) {  // -Z face
      logE("first renderProj failed\n");
      return 1;
    }
    if (renderProj(cmd, projPass2, projPipe2,
                   *projDescriptor.at(framebuf_i * 2 + 1), framebuf_i) ||
        writeToCubemap(cmd, 2, 0) ||  // +Y face (top)
        writeToCubemap(cmd, 3, 2) ||  // -Y face (bottom)
        cmd.barrier(*cubemap.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
      logE("second renderProj failed\n");
      return 1;
    }
    return 0;
  }

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
    vert::st_11hdr_vert sv[] = {
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
    // Load scene from .assbin file
    const aiScene* scene;
    AssimpGlue assimp;
    if (assimp.import("11model.assbin", &scene)) {
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
        vertices.emplace_back(vert::st_11hdr_vert{
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

  int buildPass() {
    // Create a small fake cubemap so frag shader has its input.
    // Only when codec.sampler.image is loaded can the real dims be found.
    if (constructCubemap(16, 16)) {
      logE("constructCubemap failed\n");
      return 1;
    }
    // Change from VK_COMPARE_OP_LESS to LESS_OR_EQUAL because the skybox is
    // drawn at Z = 1.0, the same as the empty (cleared) depth buffer.
    pipe0.info().depthsci.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    pipe0.info().rastersci.cullMode = VK_CULL_MODE_NONE;
    pipe0.info().perFramebufColorBlend.at(0) =
        command::PipelineCreateInfo::withEnabledAlpha();
    auto vert = std::make_shared<command::Shader>(cpool.vk.dev);
    auto frag = make_shared<command::Shader>(cpool.vk.dev);
    return addSkyboxToVertexIndex() ||
           vert->loadSPV(spv_11hdr_vert, sizeof(spv_11hdr_vert)) ||
           frag->loadSPV(spv_11hdr_frag, sizeof(spv_11hdr_frag)) ||
           uglue.shaders.add(pipe0, vert) || uglue.shaders.add(pipe0, frag) ||
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
                    std::vector<science::Sampler*>{&cubemap})) {
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
    if (cmdBuffer.beginSimultaneousUse()) {
      logE("buildFramebuf: command buffer [%zu].begin failed\n", framebuf_i);
      return 1;
    }
    if (initStep >= 3) {
      if (projectToCube(cmdBuffer, framebuf_i)) {
        logE("buildFramebuf[%zu]: projectToCube failed\n", framebuf_i);
        return 1;
      }
    }
    if (cmdBuffer.beginSubpass(pass, framebuf, 0) ||
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

  float lastInitStepTime{0};
  int continueInit() {
    if (uglue.elapsed.get() - 1 < lastInitStepTime) {
      return 0;
    }
    lastInitStepTime = uglue.elapsed.get();
    if (initStep == 0) {  // Step 0: import mesh
      vertices.clear();
      uglue.indices.clear();
      if (importMesh() || addSkyboxToVertexIndex() ||
          uglue.updateVertexIndexBuffer(vertices) ||
          onResized(cpool.vk.dev.swapChainInfo.imageExtent,
                    memory::ASSUME_POOL_QINDEX)) {
        logE("importMesh or updateVertexIndexBuffer or onResized failed\n");
        return 1;
      }
      initStep++;
      return 0;
    }
    if (initStep == 1) {  // Step 1: load 11input.jpg
      const char* filename = "11input.jpg";
      if (!codec.cpu.codec && codec.open(filename)) {
        return 1;
      }
      codec.sampler.info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
      codec.sampler.info.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      auto& imgSize = codec.sampler.image->info.extent;
      if (codec.moreLines()) {
        if (codec.read()) {
          logE("read(%s) failed at line %zu\n", filename,
               (size_t)codec.cpu.lineCount);
          return 1;
        }
        logI("read(%s) complete: %zu/%zu\n", filename,
             (size_t)codec.cpu.lineCount, (size_t)imgSize.height);
        if (!codec.moreLines()) {
          initStep++;
        }
      }
      return 0;
    }
    if (initStep == 2) {  // Step 2: allocate proj and cubemap
      auto& dev = cpool.vk.dev;
      proj = std::make_shared<memory::Image>(dev);

      proj->info = codec.sampler.image->info;
      proj->info.format = dev.swapChainInfo.imageFormat;
      proj->info.extent.width = codec.sampler.image->info.extent.width * 2;
      proj->info.extent.height = codec.sampler.image->info.extent.height;
      proj->info.mipLevels = 1;
      proj->info.usage =
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

      auto vert = std::make_shared<command::Shader>(dev);
      auto frag = std::make_shared<command::Shader>(dev);

      // Build projectToCube() projPass.
      projPipe1.info().dynamicStates.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
      projPipe1.info().dynamicStates.emplace_back(VK_DYNAMIC_STATE_SCISSOR);

      VkViewport& viewport = projPipe1.info().viewports.at(0);
      viewport.width = (float)proj->info.extent.width;
      viewport.height = (float)proj->info.extent.height;

      projPipe1.info().scissors.at(0).extent.width = proj->info.extent.width;
      projPipe1.info().scissors.at(0).extent.height = proj->info.extent.height;
      // Instead of 6 vertices to produce 2 triangles in a rect, just use 4:
      projPipe1.info().asci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

      if (projPass1.setTargetImage(proj) || projPass2.setTargetImage(proj) ||
          vert->loadSPV(spv_11sphere2cube_vert,
                        sizeof(spv_11sphere2cube_vert)) ||
          frag->loadSPV(spv_11sphere2cube_frag,
                        sizeof(spv_11sphere2cube_frag)) ||
          proj->ctorAndBindDeviceLocal()) {
        logE("projectToCube: shaders or projPass1.ctor failed\n");
        return 1;
      }

      projPipe2.info() = projPipe1.info();
      if (projShaders.add(projPipe1, vert) ||
          projShaders.add(projPipe1, frag) ||
          // Must call add to projPipe2 also because it points to projPass2.
          // Rebuilding projPass1 between the two pipes would be slower.
          projShaders.add(projPipe2, vert) ||
          projShaders.add(projPipe2, frag) ||
          projShaders.finalizeDescriptorLibrary(projDescriptorLibrary)) {
        logE("projShaders or projDescriptorLibrary failed\n");
        return 1;
      }

      if (constructCubemap(proj->info.extent.width / 4,
                           proj->info.extent.height)) {
        logE("constructCubemap failed\n");
        return 1;
      }
      projDescriptor.clear();
      for (size_t i = 0; i < uglue.uniform.size() * 2; i++) {
        std::shared_ptr<memory::DescriptorSet> ds =
            projDescriptorLibrary.makeSet(0);
        if (!ds) {
          logE("projDescriptorLibrary.makeSet failed %zu of %zu\n", i,
               uglue.uniform.size() * 2);
          return 1;
        }
        projDescriptor.push_back(ds);
      }

      // Specialization constants for fragment shader.
      sphere2cubefrag::SpecializationConstants spec;
      spec.OUT_W = (float)proj->info.extent.width;
      spec.OUT_H = (float)proj->info.extent.height;
      spec.SAMPLER_W = codec.sampler.image->info.extent.width;
      spec.SAMPLER_H = codec.sampler.image->info.extent.height;
      // Set up projPass1 with SIDES==true
      spec.SIDES = true;
      if (projPipe1.info().specialize(spec)) {
        logE("first specialize failed\n");
        return 1;
      }

      // Set up projPass2 with SIDES==false
      spec.SIDES = false;
      if (projPipe2.info().specialize(spec)) {
        logE("second spec.addToStage failed\n");
        return 1;
      }

      if (projPass1.setName("projPass1") || projPass1.ctorError()) {
        logE("projPass1 failed\n");
        return 1;
      }
      if (projPass2.setName("projPass2") || projPass2.ctorError()) {
        logE("projPass2 failed\n");
        return 1;
      }

      // Transition to a state as if the renderpass had just ended. Then
      // rendering always ends at the same layouts.
      {
        science::SmartCommandBuffer smart(uglue.stage.pool,
                                          uglue.stage.poolQindex);
        if (smart.ctorError() || smart.autoSubmit() ||
            smart.barrier(*proj,  // from PREINITIALIZED to:
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) ||
            smart.barrier(*codec.sampler.image,  // from TRANSFER_SRC to:
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
          logE("proj barrier to TRANSFER_SRC failed\n");
          return 1;
        }
      }

      initStep++;
      cubeDebug = 0;
      if (onResized(cpool.vk.dev.swapChainInfo.imageExtent,
                    memory::ASSUME_POOL_QINDEX)) {
        logE("onResized at initStep=%d failed\n", initStep);
        return 1;
      }
      return 0;
    }
    if (initStep == 3) {
      return 0;
    }
    return 0;
  }

  int redraw(std::shared_ptr<memory::Flight>& flight) {
    auto& io = ImGui::GetIO();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(5, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(800, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Config");
    ImGui::Text("%.0ffps", io.Framerate);
    float sliderWidth = ImGui::GetWindowWidth() - ImGui::GetFontSize();
    ImGui::Text("turn:");
    ImGui::PushItemWidth(sliderWidth);
    ImGui::SliderFloat("turn_key", &turn, 0.0f, 1.0f, "%.3f");
    ImGui::PopItemWidth();
    ImGui::End();

    float px = uglue.fonts.at(0)->SizePixels;
    ImGui::SetNextWindowPos(ImVec2(64, 64 + 3 * px), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(700, 200), ImGuiCond_FirstUseEver);
    if (igloo.render()) {
      return 1;
    }

    auto& ubo = *reinterpret_cast<vert::UniformBufferObject*>(flight->mmap());
    auto view = glm::lookAt(glm::vec3(-20.0f, 0.0f, 0.0f),  // Object pose.
                            glm::vec3(0.0f, 0.0f, 0.0f),    // Camera pose.
                            glm::vec3(0.0f, 1.0f, 0.0f));   // Up vector.

    ubo.lightPos = glm::vec4(0, 2, 1, 0);
    ubo.modelview = view * glm::mat4_cast(modelOrient);

    ubo.proj = glm::perspective(glm::radians(45.0f), cpool.vk.dev.aspectRatio(),
                                0.1f, 100.0f);
    ubo.proj[1][1] *= -1;  // Convert from OpenGL to Vulkan by flipping Y.
    ubo.cubeDebug = cubeDebug;
    ubo.turn = turn;

    command::SubmitInfo info;
    if (uglue.submit(flight, info)) {
      logE("uglue.submit failed\n");
      return 1;
    }
    return 0;
  }

 public:
  int run() {
    uglue.fonts.at(0)->SizePixels = 18.f;

    if (cpool.ctorError() || uglue.imGuiInit() || buildPass() ||
        igloo.ctorError() ||
        uglue.descriptorLibrary.setName("uglue.descriptorLibrary")) {
      return 1;
    }

    while (!uglue.windowShouldClose()) {
      UniformGlue::onGLFWRefresh(uglue.window);  // Calls redraw().
      if (uglue.redrawErrorCount > 0) {
        return 1;
      }

      if (initStep < maxInitStep && continueInit()) {
        return 1;
      }
    }
    // A normal shutdown does not need to print any logs. Clear igloo.
    igloo.clear();
    return cpool.deviceWaitIdle();
  }
};

static int createApp(GLFWwindow* window) {
  VolcanoLogFn prevLogger = logVolcano;
  ImGuiLogOutputOverride::captureLogs();
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
    logE("inst.ctorError or inst.open failed\n");
  } else if (!inst.devs.size()) {
    logE("No vulkan devices found (or driver missing?)\n");
  } else {
    r = std::make_shared<Example11>(inst, window, prevLogger)->run();
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
  GLFWwindow* window = glfwCreateWindow(800, 600, "11hdr Vulkan window",
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
