/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This runs a libretro frontend. See https://docs.libretro.com
 */

#include "imgui.h"
#include "include/libretro.h"
#include "retroglfw.h"
#include "retrojni.h"

// uniform_glue.h has already #included some glm headers.
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <fcntl.h>

#ifdef __ANDROID__
#include <android/keycodes.h>
#include <android_native_app_glue.h> /* for struct android_app */
#endif                               /*__ANDROID__*/

#include <ao_glue/ao_glue.h>

#include "10cubemap/10retro/10retro.frag.h"
#include "10cubemap/10retro/10retro.vert.h"

namespace example {

#include "10cubemap/10retro/struct_10retro.vert.h"

namespace frag {
#include "10cubemap/10retro/struct_10retro.frag.h"
}

const std::vector<st_10retro_vert> vertices = {
    {{0.f, 0.f, 0.0f}, {0.0f, 0.0f}},
    {{1.f, 0.f, 0.0f}, {1.0f, 0.0f}},
    {{1.f, 1.f, 0.0f}, {1.0f, 1.0f}},
    {{0.f, 1.f, 0.0f}, {0.0f, 1.0f}}};

const std::vector<uint32_t> indices = {
    0, 3, 2,  // Triangle 1 uses vertices[0] - vertices[3]
    2, 1, 0,  // Triangle 2 uses vertices[0] - vertices[2]
};

const int INIT_WIDTH = 800, INIT_HEIGHT = 600;

class App : public BaseApplication, public RetroUIinterface {
 public:
  static constexpr size_t sampleLayout = 0;
  App(language::Instance& instance, GLFWwindow* window)
      : BaseApplication{instance},
        uglue{*this, window, sampleLayout, bindingIndexOfUniformBufferObject(),
              sizeof(UniformBufferObject)},
        ui(uglue, retroweb) {
    resizeFramebufListeners.emplace_back(std::make_pair(
        [](void* self, language::Framebuf& framebuf, size_t fbi,
           size_t) -> int {
          return static_cast<App*>(self)->buildFramebuf(framebuf, fbi);
        },
        this));
    uglue.redrawListeners.emplace_back(std::make_pair(
        [](void* self, std::shared_ptr<memory::Flight>& flight) -> int {
          return static_cast<App*>(self)->redraw(flight);
        },
        this));
    wantApp = retroweb.getApps().end();
    self = this;
  }

  UniformGlue uglue;
  RetroWeb retroweb;
  RetroGLFW ui;
  ao_glue ao;
  std::shared_ptr<ao_dev> aud;
  // emuFrame is one Sampler per command buffer. It must be kept in sync with
  // ui.wantEx and ui.emuFrameFmt.
  std::vector<science::Sampler> emuFrame;
  std::shared_ptr<memory::Flight> videoFlight;
  std::string gameMessage;
  unsigned gameMessageFramesLeft{0};
  bool videoSameAsPrev{false};

  // videoRefresh is called if the core does not setup a different video output
  // method. This is the default method: data contains image pixels.
  // Vulkan formats include an alpha channel, but data's byte in the alpha
  // channel *must* be overwritten with a constant.
  // This sample replaces alpha in the fragment shader, 10retro.frag.
  virtual void videoRefresh(const void* data, unsigned width, unsigned height,
                            size_t pitch, VkFormat format) override {
    if (videoRefreshError(data, width, height, pitch, format)) {
      uglue.redrawErrorCount++;
    }
  }

  int videoRefreshError(const void* data, unsigned width, unsigned height,
                        size_t pitch, VkFormat format) {
    if (width != ui.wantEx.width || height != ui.wantEx.height ||
        format != ui.emuFrameFmt) {
      // Change wantEx. Main loop will notice and rebuild command buffers.
      ui.wantEx.width = width;
      ui.wantEx.height = height;
      // Update window aspect ratio lock.
      ui.setUserFreeResize(ui.userFreeResize);
      ui.emuFrameFmt = format;
      // Device must be idled and everything rebuilt. Discard this frame.
      return 0;
    }

    auto& sampler = emuFrame.at(uglue.getImage());
    auto& info = sampler.image->info;
    if (width != info.extent.width || height != info.extent.height) {
      logE("emuFrame[%u] got %u x %u want %u x %u\n", uglue.getImage(),
           info.extent.width, info.extent.height, width, height);
      return 1;
    }
    VkDeviceSize bpp = 0;
    switch (info.format) {
      case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
      case VK_FORMAT_B5G6R5_UNORM_PACK16:
        bpp = 2;
        break;
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_R8G8B8A8_SRGB:
        bpp = 4;
        break;
      default:
        logE("emuFrame[%u] unsupported format %u\n", uglue.getImage(),
             info.format);
        return 1;
    }
    VkDeviceSize frameSize = info.extent.width * info.extent.height * bpp;
    if (videoFlight) {
      // The core generated another frame before the render loop asked it
      // for more frames. In other words, throw the previous frame away.
      if (!data) {
        // data == NULL means "duplicate prev frame." Collapse to 1 frame.
        return 0;
      }
      if (videoFlight->end()) {
        logE("overwrite emuFrame %u: end failed\n", uglue.getImage());
        uglue.redrawErrorCount++;
        return 0;
      }
      videoFlight.reset();
    } else if (!data) {
      // data == NULL means "duplicate prev frame."
      videoSameAsPrev = true;
      return 0;
    }
    if (uglue.stage.mmap(*sampler.image, frameSize, videoFlight)) {
      logE("uglue.stage.mmap(emuFrame %u) failed\n", uglue.getImage());
      return 1;
    }
    if (ui.emuFrameFmt == VK_FORMAT_A1R5G5B5_UNORM_PACK16 &&
        info.format == VK_FORMAT_R8G8B8A8_SRGB) {
      const uint16_t* src = reinterpret_cast<const uint16_t*>(data);
      uint32_t* dst = reinterpret_cast<uint32_t*>(videoFlight->mmap());
      for (unsigned y = 0; y < height; y++, src += pitch / 2) {
        const uint16_t* p = src;
        for (unsigned x = 0; x < width; x++) {
          // Convert one pixel at a time on CPU. Would benefit from SSE/Neon.
          // Best would be to transfer to GPU and convert there.
          uint32_t pixel = *(p++);
          *(dst++) = ((pixel & 0x1f) | ((pixel & 0x3e0) << 3) |
                      ((pixel & 0x7c00) << 6))
                     << 3;
        }
      }
    } else if (ui.emuFrameFmt == VK_FORMAT_B5G6R5_UNORM_PACK16 &&
               info.format == VK_FORMAT_R8G8B8A8_SRGB) {
      const uint16_t* src = reinterpret_cast<const uint16_t*>(data);
      uint32_t* dst = reinterpret_cast<uint32_t*>(videoFlight->mmap());
      for (unsigned y = 0; y < height; y++, src += pitch / 2) {
        const uint16_t* p = src;
        for (unsigned x = 0; x < width; x++) {
          // Convert one pixel at a time on CPU. Would benefit from SSE/Neon.
          // Best would be to transfer to GPU and convert there.
          uint32_t pixel = *(p++);
          *(dst++) = ((pixel & 0x1f) | ((pixel & 0x7e0) << 2) |
                      ((pixel & 0xf800) << 5))
                     << 3;
        }
      }
    } else if (ui.emuFrameFmt != info.format) {
      logE("emuFrameFmt %s %u does not match info.format %s %u\n",
           string_VkFormat(ui.emuFrameFmt), ui.emuFrameFmt,
           string_VkFormat(info.format), info.format);
      return 1;
    } else {
      const uint8_t* src = reinterpret_cast<const uint8_t*>(data);
      uint8_t* dst = reinterpret_cast<uint8_t*>(videoFlight->mmap());
      width *= bpp;
      for (unsigned y = 0; y < height; y++) {
        memcpy(dst, src, width);
        src += pitch;
        dst += width;
      }
    }

    videoFlight->copies.resize(1);
    VkBufferImageCopy& copy = videoFlight->copies.at(0);
    copy.bufferOffset = 0;
    copy.bufferRowLength = info.extent.width;
    copy.bufferImageHeight = info.extent.height;
    copy.imageSubresource = sampler.image->getSubresourceLayers(0);
    copy.imageOffset = {0, 0, 0};
    copy.imageExtent = {copy.bufferRowLength, copy.bufferImageHeight, 1};
    if (videoFlight->canSubmit()) {
      if (uglue.stage.flushButNotSubmit(videoFlight)) {
        logE("updateEmuFrame: uglue.stage.flushButNotSubmit failed\n");
        return 1;
      }
      if (videoFlight->barrier(*sampler.image,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
        logE("updateEmuFrame: flight->barrier failed\n");
        return 1;
      }
    } else {
      logE("updateEmuFrame: need a commandbuffer sitting around\n");
      logE("updateEmuFrame: for barrier to LAYOUT_SHADER_READ_ONLY\n");
      return 1;
    }
    return 0;
  }

  bool audioMute{false};
  virtual size_t audioBatch(const int16_t* data, size_t frames,
                            int rate) override {
    if (audioMute) {
      return frames;
    }
    if (!aud || rate != ui.emuRate) {
      if (ao.live().empty()) {
        logE("ao.live: no device found\n");
        audioMute = true;
        return frames;
      }
      aud.reset();  // Close prev ao_dev in aud to switch sample rates.
      ao_sample_format format;
      memset(&format, 0, sizeof(format));
      format.bits = 16;
      format.rate = rate;
      format.channels = 2;
      format.byte_format = AO_FMT_NATIVE;
      auto ld = ao.live().at(0);
      aud = ao.open(ld, format);
      if (!aud) {
        logE("ao.open failed\n");
        audioMute = true;
        return frames;
      }
      ui.emuRate = rate;
      logI("audio: %s @ %dhz\n", ld->short_name, ui.emuRate);
    }
    if (aud->play((char*)const_cast<int16_t*>(data),
                  frames * 2 * sizeof(data[0]))) {
      logE("audioBatch: play failed\n");
    }
    // Can return less than frames, if buffer is full.
    return frames;
  }

  // showMessage is used by some games.
  virtual void showMessage(const char* msg, unsigned frames) override {
    gameMessage = msg;
    if (frames < 60) {
      frames = 60;
    }
    gameMessageFramesLeft = frames;
  }

  // setPerfLevel can happen any time during a game. The values are arbitrary,
  // but higher means a more demanding emulation task.
  virtual void setPerfLevel(unsigned lvl) override {
    if (ui.curApp == retroweb.getApps().end()) {
      return;
    }
    ui.curApp->second.setPerfLevel(lvl);
    if (lvl > 10 && !gameMessageFramesLeft) {
      char buf[16];
      snprintf(buf, sizeof(buf), "%u", lvl);
      gameMessage = "Requires \"level ";
      gameMessage += buf;
      gameMessage += "\" hardware";
      gameMessageFramesLeft = 120;
    }
  }

 protected:
  UniformBufferObject ubo;
  science::PipeBuilder pipe0{pass};

  unsigned startC{0};
  std::vector<char> defaultFrame;
  void setEmuFrameExDefault() {
    ui.wantEx.width = 320;  // Just pick a size.
    ui.wantEx.height = 240;
    ui.emuFrameFmt = VK_FORMAT_R8G8B8A8_SRGB;

    VkDeviceSize bpp = 4;
    defaultFrame.resize(ui.wantEx.width * ui.wantEx.height * bpp);
    uint32_t* dst = reinterpret_cast<uint32_t*>(defaultFrame.data());
    uint32_t c = startC, d = 1, i = 0;
    for (; i < 320 * 120; i++) {
      *(dst++) = (c += d) & ~0x7f;
    }
    d = -d;
    c += 0x400000;
    for (; i < ui.wantEx.height * ui.wantEx.width; i += 320) {
      c += 0x10000;
      for (size_t j = 0; j < 320; j++) {
        *(dst++) = (c += d) & ~0x7f;
      }
    }
  }

  int initEmuFrame(size_t frameIndex) {
    if (emuFrame.size() <= frameIndex) {
      emuFrame.emplace_back(cpool.vk.dev);
    }
    science::Sampler& sampler = emuFrame.at(frameIndex);
    if (sampler.vk && sampler.image->info.extent.width == ui.wantEx.width &&
        sampler.image->info.extent.height == ui.wantEx.height &&
        sampler.image->info.format == ui.emuFrameFmt) {
      return 0;  // Window resize should not trigger sampler.ctorError.
    }
    VkFormatFeatureFlags flags =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (cpool.vk.dev.apiVersionInUse() >= VK_MAKE_VERSION(1, 1, 0)) {
      flags |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
               VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    }
    std::vector<VkFormat> formatChoices;
    formatChoices.push_back(ui.emuFrameFmt);
    switch (ui.emuFrameFmt) {
      case VK_FORMAT_R8G8B8A8_SRGB:
        // Allow downgrade from _SRGB to _UNORM:
        formatChoices.push_back(VK_FORMAT_R8G8B8A8_UNORM);
        break;
      case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
      case VK_FORMAT_B5G6R5_UNORM_PACK16:
        // macOS desktop devices do not support "packed 16-bit" formats, see
        // https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf
        // (iOS devices do though!). If this gets chosen, must convert to it.
        formatChoices.push_back(VK_FORMAT_R8G8B8A8_SRGB);
        break;
      default:
        logE("initEmuFrame(%zu) unknown format %s %u\n", frameIndex,
             string_VkFormat(ui.emuFrameFmt), ui.emuFrameFmt);
        return 1;
    }
    sampler.image->info.format = cpool.vk.dev.chooseFormat(
        sampler.image->info.tiling, flags, VK_IMAGE_TYPE_2D, formatChoices);
    if (sampler.image->info.format == VK_FORMAT_UNDEFINED) {
      logE("initEmuFrame: no format supports BLIT_DST\n");
      return 1;
    }
    sampler.image->info.extent.width = ui.wantEx.width;
    sampler.image->info.extent.height = ui.wantEx.height;
    sampler.image->info.extent.depth = 1;
    sampler.image->info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (sampler.image->setMipLevelsFromExtent()) {
      logE("initEmuFrame: setMipLevelsFromExtent failed\n");
      return 1;
    }
    sampler.info.maxLod = sampler.image->info.mipLevels;
    sampler.info.anisotropyEnable = false ? VK_TRUE : VK_FALSE;
    sampler.info.magFilter = false ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sampler.info.minFilter = false ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sampler.info.mipmapMode =
        false ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (sampler.ctorError()) {
      logE("initEmuFrame: sampler.ctorError failed\n");
      return 1;
    }
    science::SmartCommandBuffer smart(cpool, uglue.stage.poolQindex);
    if (smart.ctorError() || smart.autoSubmit() ||
        smart.barrier(*sampler.image,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
      logE("initEmuFrame: barrier(SHADER_READ_ONLY) failed\n");
      return 1;
    }
    return 0;
  }

  int buildPass() {
    uglue.indices = indices;
    auto vert = std::make_shared<command::Shader>(cpool.vk.dev);
    auto frag = std::make_shared<command::Shader>(cpool.vk.dev);
    setEmuFrameExDefault();
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
           vert->loadSPV(spv_10retro_vert, sizeof(spv_10retro_vert)) ||
           frag->loadSPV(spv_10retro_frag, sizeof(spv_10retro_frag)) ||
           uglue.shaders.add(pipe0, vert) || uglue.shaders.add(pipe0, frag) ||
           uglue.initPipeBuilderFrom(pipe0, vertices) ||
           uglue.buildPassAndTriggerResize();
  }

  int buildFramebuf(language::Framebuf& framebuf, size_t framebuf_i) {
    char name[256];
    auto& ds = *uglue.descriptorSet.at(framebuf_i);
    snprintf(name, sizeof(name), "uglue.descriptorSet[%zu]", framebuf_i);
    if (initEmuFrame(framebuf_i) || ds.setName(name) ||
        ds.write(frag::bindingIndexOftexSampler(),
                 std::vector<science::Sampler*>{&emuFrame.at(framebuf_i)})) {
      logE("uglue.descriptorSet[%zu] failed\n", framebuf_i);
      return 1;
    }
    ui.setUBOdirty(framebuf_i, 1);

    auto& newSize = cpool.vk.dev.swapChainInfo.imageExtent;
    VkViewport& viewport = pipe0.info().viewports.at(0);
    viewport.width = (float)newSize.width;
    viewport.height = (float)newSize.height;
    pipe0.info().scissors.at(0).extent = newSize;
    VkBuffer vertBufs[] = {uglue.vertexBuffer.vk};
    VkDeviceSize offsets[] = {0};

    auto& cmdBuffer = uglue.cmdBuffers.at(framebuf_i);
    if (cmdBuffer.beginSimultaneousUse() ||
        cmdBuffer.beginSubpass(pass, framebuf, 0) ||
        cmdBuffer.bindGraphicsPipelineAndDescriptors(*pipe0.pipe, 0, 1,
                                                     &ds.vk) ||
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

  GLFWfullscreen fs;
#ifdef __APPLE__
  // macOS 10.7 "spaces" maximized window
  bool is10_7_max{false};
#endif /*__APPLE__*/
  // wantNormal is if the user wants to switch to windowed mode.
  bool wantNormal{false};
  // wantMax is if the user wants to switch to maximized mode.
  int wantMax{0};
  // wantMonitor is if the user wants fullscreen (or to switch monitors).
  GLFWmonitor* wantMonitor{nullptr};
  // wantApp is set to an app if the user chooses to launch one.
  mapStringToApp::iterator wantApp;
  // imguiRanAtLeastOnce helps keep the ImGui window visible on-screen.
  bool imguiRanAtLeastOnce{false};

  int doAppSwitching() {
    auto& apps = retroweb.getApps();
    if (wantApp == apps.end()) {
      return 0;
    }
    if (ui.curApp != apps.end() && ui.curApp->second.close()) {
      logE("app[%s].close failed\n", ui.curApp->first.c_str());
      wantApp = apps.end();
      return 1;
    }
    wantApp->second.core.keyToJoypad.clear();
    // clang-format off
#define MAP(a) wantApp->second.core.keyToJoypad.emplace_back( \
                  std::make_pair((int)RETROK_##a, RETRO_DEVICE_ID_JOYPAD_##a));
#define MAP2(a, b) wantApp->second.core.keyToJoypad.emplace_back(std:: \
                       make_pair((int)RETROK_##a, RETRO_DEVICE_ID_JOYPAD_##b));
    MAP(UP) MAP(DOWN) MAP(LEFT) MAP(RIGHT)
    MAP2(a, Y) MAP2(d, X) MAP2(RSHIFT, SELECT)
    MAP2(z, B) MAP2(c, A) MAP2(RETURN, START)
#undef MAP
#undef MAP2
        // clang-format on
        if (wantApp->second.open()) {
      logE("app[%s].open failed\n", wantApp->first.c_str());
      ui.curApp = retroweb.getApps().end();
      setEmuFrameExDefault();
      uglue.needRebuild = true;
      if (!gameMessageFramesLeft) {
        gameMessage = wantApp->first;
        gameMessage += " failed to load";
        gameMessageFramesLeft = 120;
      }
      wantApp = apps.end();
      return 0;
    }
    ui.curApp = wantApp;
    ui.doPause();
    wantApp = apps.end();
    return 0;
  }

  int updateGUImainWindowApps() {
    auto& apps = retroweb.getApps();
    wantApp = apps.end();
    if (apps.empty()) {
      ImGui::Text("%s", "No apps loaded.");
      ImGui::Text("%s", "Click on settings,");
      ImGui::Text("%s", "then \"download apps\"");
      ImGui::Text("%s", "to get some free apps");
      ImGui::Text("%s", "for RetroArch.");
      ImGui::Text("%s", "");
    } else {
      for (auto i = apps.begin(); i != apps.end(); i++) {
        bool selected = i == ui.curApp;
        bool want = ImGui::RadioButton(i->first.c_str(), selected);
        if (!selected && want && wantApp == apps.end()) {
          wantApp = i;
        }
      }
    }
    return 0;
  }

  int updateGUImainWindowSettings() {
    bool want = false;
    if (retroweb.isSyncRunning()) {
      want = ImGui::Button("stop download");
      if (want) {
        if (retroweb.stop()) {
          logE("retroweb.stop failed\n");
          return 1;
        }
      }
    } else {
      want = ImGui::Button(retroweb.getApps().empty() ? "download apps"
                                                      : "update apps");
      if (want) {
        if (retroweb.startSync()) {
          logE("retroweb.startSync failed\n");
          return 1;
        }
      }
    }
    if (!fs.isNormal(uglue.window) && !ui.userFreeResize) {
      ui.setUserFreeResize(true);
    }

    wantNormal = false;
    wantMax = 0;
    wantMonitor = nullptr;
#ifdef __ANDROID__
    bool wantNormal =
        ImGui::RadioButton("statusbar", fs.isNormal(uglue.window));
#else  /*__ANDROID__*/
    want = ImGui::RadioButton("no black bars",
                              fs.isNormal(uglue.window) && !ui.userFreeResize);
    if (want) {
      wantNormal = true;
      ui.setUserFreeResize(false);
    }
    want = ImGui::RadioButton("black bars (resizable)",
                              fs.isNormal(uglue.window) && ui.userFreeResize);
    if (want) {
      wantNormal = true;
      ui.setUserFreeResize(true);
    }
#endif /*__ANDROID__*/

#ifdef __APPLE__
    is10_7_max = fs.isLionMax(uglue.window);
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
        snprintf(text, sizeof(text), "%s on %zu:%s", fs.getFullscreenType(), i,
                 it->name.c_str());
        if (ImGui::RadioButton(text,
                               fs.isFullscreen(uglue.window) &&
                                   fs.getMonitor(uglue.window) == it->mon)) {
          wantMonitor = it->mon;
        }
      }
    }

#ifndef __ANDROID__
    if (ImGui::RadioButton("maximized window", fs.isMaximized(uglue.window))) {
      wantMax = 1;
    }
#endif
    return 0;
  }

  std::string wantSave;
  std::string longPress;
  int longPressFrameCount{0};
  std::string renameSaveFrom;
  char renameSaveTo[256];
  void updateGUImainWindowSavesLongPressCheck(std::string id) {
    if (longPress == id && ImGui::GetFrameCount() - longPressFrameCount > 90) {
      longPress.clear();
      renameSaveFrom = id;
      strncpy(renameSaveTo, id.c_str(), sizeof(renameSaveTo) - 1);
      renameSaveTo[sizeof(renameSaveTo) - 1] = 0;
      // Leave longPressFrameCount non-zero to move focus next frame.
#ifdef __ANDROID__
      glfwShowSoftInput(1);
#endif /*__ANDROID__*/
    }
  }

  int doLoadFromSave() {
    if (wantSave.empty()) {
      return 0;
    }
    std::string localName = wantSave;
    wantSave.clear();  // if wantSave remained set, app would get stuck.
    retroweb.ignoreFirstLoadStat = localName == ".last";
    std::string appForSave;
    if (retroweb.loadSave(ui.curApp, localName, appForSave)) {
      retroweb.ignoreFirstLoadStat = false;
      return 0;  // loadSave failed. User can look in the logs for why.
    }
    retroweb.ignoreFirstLoadStat = false;
    if (!appForSave.empty()) {
      auto p = retroweb.getApps().find(appForSave);
      if (p == retroweb.getApps().end()) {
        logW("doLoadFromSave: could not find app \"%s\"\n", appForSave.c_str());
        return 0;
      }
      wantApp = p;
      if (doAppSwitching()) {
        return 1;
      }
      // With ui.curApp now correct, call loadSave() again.
      std::string secondTry;
      if (retroweb.loadSave(ui.curApp, localName, secondTry)) {
        return 0;  // loadSave failed even after switching apps.
      }
      if (!secondTry.empty()) {
        logE("loadSave: asked for \"%s\" but then asked for \"%s\"\n",
             appForSave.c_str(), secondTry.c_str());
        return 1;
      }
    }
    ui.doPause();  // Pause after loading save.
    return 0;
  }

  int cleanShutdown() {
    if (ui.curApp == retroweb.getApps().end()) {
      return 0;
    }
    // Mostly Android needs to gracefully handle shutdown without
    // trashing what the user was doing.
    std::string name = ".last";
    if (retroweb.saveTo(ui.curApp->second, name)) {
      // Do nothing on failure.
    }
    return 0;
  }

  int updateGUImainWindowSaves() {
#ifdef __ANDROID__
    if (ImGui::Button("Make \"autosave\"###saveTo") &&
        ui.curApp != retroweb.getApps().end()) {
      if (retroweb.saveTo(ui.curApp->second, "autosave")) {
        // Do nothing on failure.
        if (ui.curApp->second.core.isLastSaveTooEarly()) {
          // Could try saving again later.
        }
      }
    }
#else  /*__ANDROID__*/
    ImGui::Text("F2 = instant autosave");
#endif /*__ANDROID__*/
    ImGui::Text("Long press = rename");
    ImGui::Separator();
    if (retroweb.saves.empty()) {
      ImGui::Text("(no saves found)");
      return 0;
    }
    // Make a localSaves copy because this code can alter the saves list.
    std::vector<std::string> localSaves;
    for (auto i = retroweb.saves.begin(); i != retroweb.saves.end(); i++) {
      if (i->substr(0, 1) != ".") {
        localSaves.emplace_back(*i);
      }
    }
    for (auto i = localSaves.begin(); i != localSaves.end(); i++) {
      if (!renameSaveFrom.empty() && *i == renameSaveFrom) {
        ImGui::InputText(("###" + renameSaveFrom).c_str(), renameSaveTo,
                         sizeof(renameSaveTo),
                         ImGuiInputTextFlags_CharsNoBlank |
                             ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          static const char* banned = "/\\:*?\"<>|";
          if (renameSaveFrom != renameSaveTo) {
            if (retroweb.saves.find(renameSaveTo) != retroweb.saves.end()) {
              gameMessage = "The save \"";
              gameMessage += renameSaveTo;
              gameMessage += "\" already exists.";
              gameMessageFramesLeft = 1800;
            } else if (strcspn(renameSaveTo, banned) != strlen(renameSaveTo)) {
              gameMessage = "Name cannot contain ";
              gameMessage += banned;
              gameMessageFramesLeft = 1800;
            } else if (renameSaveTo[0] == '.') {
              gameMessage = "Name cannot start with .";
              gameMessageFramesLeft = 1800;
            } else if (retroweb.renameSave(renameSaveFrom, renameSaveTo)) {
              logE("rename \"%s\" to \"%s\" failed\n", renameSaveFrom.c_str(),
                   renameSaveTo);
            }
          }
          renameSaveFrom.clear();
#ifdef __ANDROID__
          glfwShowSoftInput(0);
#endif /*__ANDROID__*/
        } else if (longPressFrameCount) {
          ImGui::SetKeyboardFocusHere();
          longPressFrameCount = 0;
        } else if (!ImGui::IsItemActive()) {
          renameSaveFrom.clear();  // User clicked elsewhere to cancel.
#ifdef __ANDROID__
          glfwShowSoftInput(0);
#endif /*__ANDROID__*/
        }
        continue;
      }
      bool want = ImGui::Button(i->c_str());
      if (want) {
        updateGUImainWindowSavesLongPressCheck(*i);
        if (renameSaveFrom.empty()) {
          wantSave = *i;
        }
      } else if (ImGui::IsItemActive() && ImGui::IsItemHovered()) {
        if (longPress != *i) {
          longPress = *i;
          longPressFrameCount = ImGui::GetFrameCount();
        } else {
          updateGUImainWindowSavesLongPressCheck(*i);
        }
      } else if (longPress == *i) {
        longPress.clear();
      }
    }
    return 0;
  }

  bool wasAppsTabSelected{false};

  int updateGUImainWindow() {
    auto& io = ImGui::GetIO();
    char fpsbuf[256];
    snprintf(fpsbuf, sizeof(fpsbuf), "%.0ffps", io.Framerate);
#ifndef _WIN32
    static constexpr uint32_t nvidiaID = 0x10de;  // NVIDIA PCI device ID.
    if (cpool.vk.dev.physProp.properties.vendorID == nvidiaID &&
        io.Framerate > 120.) {
      // Workaround NVidia bug in drivers before 425.62 (fixed before 430.64)
      usleep(1e6 / 120. - 1e6 / io.Framerate);  // Do a rough framerate-limiter.
    }
#endif
    {
      std::string title("Retro ");
      title += fpsbuf;
      glfwSetWindowTitle(uglue.window, title.c_str());
    }
#ifdef __ANDROID__
    if (!ui.mainWindowOpen) {
      // Android: can re-open window without moving the "mouse." Window just
      // closed, i.e. "mouse" was released, deselect close button.
      io.MousePos.x = 0;
      io.MousePos.y = 0;
    }
    if ((ImGui::GetFrameCount() % 120) == 60) {
      logI("%s\n", fpsbuf);
    }
#endif                        /*__ANDROID__*/
    if (!ui.mainWindowOpen) {
      return 0;
    }
    ImGui::SetNextWindowPos(ImVec2(64, 64), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 300), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("RetroArch", &ui.mainWindowOpen, 0)) {
      ImGui::End();
      return 0;
    }

    if (imguiRanAtLeastOnce) {
      auto& imageExtent = cpool.vk.dev.swapChainInfo.imageExtent;
      glm::vec2 ex(imageExtent.width / io.DisplayFramebufferScale.x,
                   imageExtent.height / io.DisplayFramebufferScale.y);
      glm::mat2 rot;
      glm::vec2 translate;
      uglue.getImGuiRotation(rot, translate);
      ex = ex * rot;
      ex.x = fabsf(ex.x);
      ex.y = fabsf(ex.y);

      auto winPos = ImGui::GetWindowPos();
      auto winSize = ImGui::GetWindowSize();
      bool clipped = false;
      // Trim ImGui window (causes a scroll bar to appear) if it overflows.
      // NOTE: It won't bounce back if e.g. the window is expanded later.
      // This mostly just helps Android to not be too frustrating.
      if (winPos.x + winSize.x > ex.x) {
        winSize.x = ex.x - winPos.x;
        clipped = true;
        if (winSize.x < 64) {
          winPos.x = ex.x - 64;
          winSize.x = 64;
        }
      }
      if (winPos.y + winSize.y > ex.y) {
        winSize.y = ex.y - winPos.y;
        clipped = true;
        if (winSize.y < 24) {
          winPos.y = ex.y - 24;
          winSize.y = 24;
        }
      }
      if (clipped) {
        ImGui::SetWindowPos(winPos);
        ImGui::SetWindowSize(winSize);
      }
    }

    {
      std::string msg(fpsbuf);
      msg += ' ';
      msg += retroweb.getSyncStatus();
      ImGui::ProgressBar(ui.progressFraction, ImVec2(-1, 0) /*default size*/,
                         msg.c_str());
    }
    ImGui::Text("%s", ui.curApp != retroweb.getApps().end()
                          ?
#ifdef __ANDROID__
                          "Paused - press < BACK     "
                          :  // FIXME: TV has to use BACK.
#else                        /*__ANDROID__*/
                          "Paused - press P to resume"
                          :
#endif                       /*__ANDROID__*/
                          "                          ");
    if (ImGui::BeginTabBar("tab_bar_id", ImGuiTabBarFlags_NoTooltip)) {
      if (ImGui::BeginTabItem("apps", NULL, 0)) {
        if (!wasAppsTabSelected) {
          // refresh cache when tab is switched
          retroweb.listCacheContents();
        }
        wasAppsTabSelected = true;
        if (updateGUImainWindowApps()) {
          return 1;
        }
        ImGui::EndTabItem();
      }
      ImGuiTabItemFlags flags = 0;
      if (!imguiRanAtLeastOnce && retroweb.getApps().empty()) {
        // Show "settings" on first run.
        flags = ImGuiTabItemFlags_SetSelected;
      }
      if (ImGui::BeginTabItem("settings", NULL, flags)) {
        wasAppsTabSelected = false;
        if (updateGUImainWindowSettings()) {
          return 1;
        }
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("saves", NULL, 0)) {
        wasAppsTabSelected = false;
        if (updateGUImainWindowSaves()) {
          return 1;
        }
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::End();
    return 0;
  }

  uint32_t prevI{(uint32_t)-1};
  int redraw(std::shared_ptr<memory::Flight>& flight) {
    if (copyVideoFromPrev && prevI != (uint32_t)-1) {
      // Core is not generating frames. Overwrite the stale data in
      // emuFrame with the last frame generated (prevI).
      if (pauseEmuFrame(prevI)) {
        logE("pauseEmuFrame failed\n");
        return 1;
      }
    }
    prevI = uglue.getImage();

    if (ui.updateImGuiInputs()) {
      logE("updateImGuiInputs failed\n");
      return 1;
    }
    ImGui::NewFrame();
    if (updateGUImainWindow()) {
      return 1;
    }

    if (gameMessageFramesLeft) {
      bool wantMessage = true;
      ImGui::SetNextWindowPos(ImVec2(64, 300), ImGuiCond_FirstUseEver);
      ImGui::Begin("Game", &wantMessage);
      ImGui::Text("%s", gameMessage.c_str());
      ImGui::End();
      gameMessageFramesLeft--;
      if (!wantMessage) {
        gameMessageFramesLeft = 0;
      }
    }

    // TODO: Optimize so ui.isUBOdirty(uglue.getImage()) prevents flight if
    // not needed. But! nextImage is not set until inside onGLFWRefresh()!
    if (flight) {
      auto& ubo = *reinterpret_cast<UniformBufferObject*>(flight->mmap());
      float xbar = 0;
      float ybar = 0;
      if (ui.curApp != retroweb.getApps().end() && ui.userFreeResize) {
        // Letterboxing: add black bars around image so aspect ratio is correct.
        // If wantAspect < gotAspect, use xbar, else use ybar.
        auto& imageExtent = cpool.vk.dev.swapChainInfo.imageExtent;
        float wantAspect = (float)ui.wantEx.width / (float)ui.wantEx.height;
        float gotAspect = (float)imageExtent.width / (float)imageExtent.height;
        if (wantAspect < gotAspect) {
          // What smaller imageExtent.width would make gotAspect == wantAspect?
          float wantWidth = wantAspect * (float)imageExtent.height;
          if (fabsf(imageExtent.width - wantWidth) < 2.f) {
            wantWidth = imageExtent.width;
          }
          xbar = 1. - wantWidth / imageExtent.width;
        } else {
          // What smaller imageExtent.height would make gotAspect == wantAspect?
          float wantHeight = (float)imageExtent.width / wantAspect;
          if (fabsf(imageExtent.height - wantHeight) < 2.f) {
            wantHeight = imageExtent.height;
          }
          ybar = 1. - wantHeight / imageExtent.height;
        }
      }
      ubo.model = glm::mat4(1.0f);
      ubo.view = glm::mat4(1.0f);
      ubo.proj = glm::ortho(-xbar, xbar + 1, ybar + 1, -ybar, -0.1f, 1.0f);
      ubo.proj[1][1] *= -1;  // Convert from OpenGL to Vulkan by flipping Y.
      ubo.proj[3][1] *= -1;  // Convert from OpenGL to Vulkan by flipping Y.
      ui.setUBOdirty(uglue.getImage(), 0);
    }

    command::SubmitInfo info;
    if (updateEmuFrame(info)) {
      logE("updateEmuFrame failed\n");
      return 1;
    }
    if (uglue.isAborted()) {
      // Not going to call Render() this frame, so tell Imgui that.
      ImGui::EndFrame();
    } else {
      imguiRanAtLeastOnce = true;
    }
    if (uglue.submit(flight, info)) {
      logE("uglue.submit failed\n");
      return 1;
    }
    return 0;
  }

  bool copyVideoFromPrev{false};
  int updateEmuFrame(command::SubmitInfo& submitInfo) {
    if (uglue.isAborted()) {
      // Frame was already aborted.
      return 0;
    }
    auto& ex = emuFrame.at(uglue.getImage()).image->info.extent;
    VkFormat curFrameFmt = emuFrame.at(uglue.getImage()).image->info.format;
    if ((curFrameFmt != ui.emuFrameFmt &&
         (curFrameFmt != VK_FORMAT_R8G8B8A8_SRGB ||
          (ui.emuFrameFmt != VK_FORMAT_A1R5G5B5_UNORM_PACK16 &&
           ui.emuFrameFmt != VK_FORMAT_B5G6R5_UNORM_PACK16))) ||
        ui.wantEx.width != ex.width || ui.wantEx.height != ex.height) {
      // Frame format or extent has changed. Abort and rebuild.
      uglue.abortFrame();
      uglue.needRebuild = true;
      return 0;
    }

    size_t prevErrorCount = uglue.redrawErrorCount;
    videoFlight.reset();
    if (ui.curApp == retroweb.getApps().end()) {
      setEmuFrameExDefault();
      videoRefresh(defaultFrame.data(), ui.wantEx.width, ui.wantEx.height,
                   4 * ui.wantEx.width, ui.emuFrameFmt);
      startC++;
      startC &= 0xff;
    } else if (!ui.gamePaused() || ui.pauseButRender) {
      if (ui.gamePaused() && ui.pauseButRender) {
      } else {
        copyVideoFromPrev = false;
      }
      // Run core (reset videoSameAsPrev, core may set it)
      // FIXME: run several frames if needed. Tell core to lower framerate.
      videoSameAsPrev = false;
      if (ui.curApp->second.nextFrame(*this)) {
        logE("app[%s].nextFrame failed\n", ui.curApp->first.c_str());
      }
      if (ui.gamePaused() && ui.pauseButRender && !videoSameAsPrev) {
        ui.pauseButRender--;
        if (!ui.pauseButRender) {
          copyVideoFromPrev = true;
        }
      }
      if (videoSameAsPrev) {
        // Activate the copy-from-prev code just like if the game paused.
        copyVideoFromPrev = true;
      }
    }
    if (uglue.redrawErrorCount != prevErrorCount) {
      logE("updateEmuFrame: video callback error\n");
      return 1;
    }
    if (videoFlight && videoFlight->canSubmit()) {
      command::CommandPool::lock_guard_t lock(cpool.lockmutex);
      if (videoFlight->end() || videoFlight->enqueue(lock, submitInfo)) {
        logE("updateEmuFrame: end or enqueue failed\n");
        return 1;
      }
    }
    videoFlight.reset();
    return 0;
  }

  int pauseEmuFrame(uint32_t prevI) {
    copyVideoFromPrev = false;
    science::SmartCommandBuffer cmd(cpool, uglue.stage.poolQindex);
    if (cpool.deviceWaitIdle() || cmd.ctorError() || cmd.autoSubmit()) {
      return 1;
    }
    for (uint32_t i = 0; i < emuFrame.size(); i++) {
      if (i != prevI &&
          (science::copyImage1to1(cmd, *emuFrame.at(prevI).image,
                                  *emuFrame.at(i).image) ||
           cmd.barrier(*emuFrame.at(i).image,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))) {
        return 1;
      }
    }
    return cmd.barrier(*emuFrame.at(prevI).image,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

 public:
  static App* self;
  int onGlfwIO(int fd, int eventBits) {
    return retroweb.http.onGlfwIO(fd, eventBits);
  }

  static int onGlfwIOCb(int fd, int eventBits) {
    return self->onGlfwIO(fd, eventBits);
  }

  int run() {
    glfwSetIOEventCallback(onGlfwIOCb);

    // Customize Dear ImGui with a larger font size.
    uglue.fonts.at(0)->SizePixels = 18.f;
    if (cpool.ctorError() || uglue.imGuiInit() || buildPass() ||
        uglue.descriptorLibrary.setName("uglue.descriptorLibrary") ||
        retroweb.ctorError()) {
      return 1;
    }
    // Poll monitors:
    onGLFWmonitorChange(NULL, 0);
    // Set up the wantSave state to load from what cleanShutdown() should
    // have saved. This will not error out if the save is not found.
    wantSave = ".last";

    for (;;) {
      if (retroweb.http.poll()) {
        return 1;
      }
      if (uglue.windowShouldClose()) {
        if (cleanShutdown()) {
          return 1;
        }
        break;
      }
      UniformGlue::onGLFWRefresh(uglue.window);  // Calls redraw().
      if (uglue.redrawErrorCount > 0) {
        return 1;
      }
      if (ao.ctorError()) {
        logE("ao.ctorError failed\n");
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

      // Defer window resizes until acquired frame has been presented.
      if (ui.needFitToAspect) {
        ui.needFitToAspect = false;
        if (cpool.deviceWaitIdle()) {
          logE("cpool.deviceWaitIdle() failed\n");
          return 1;
        }
        int w = 0, h = 0;
        glfwGetWindowSize(uglue.window, &w, &h);
        // This is an improved-upon "stay in the right aspect ratio" method:
        float wantArea = (float)ui.wantEx.width * (float)ui.wantEx.height;
        float scale = sqrtf((float)w * (float)h / wantArea);
        h = (int)(ui.wantEx.height * scale);
        w = (int)(ui.wantEx.width * scale);
        glfwSetWindowSize(uglue.window, w, h);
        glfwSetWindowAspectRatio(uglue.window, ui.wantEx.width,
                                 ui.wantEx.height);
        w *= uglue.scaleX;
        h *= uglue.scaleY;
        // Wait for the OS to resize the window.
        auto& newSize = cpool.vk.dev.swapChainInfo.imageExtent;
        for (double t0 = uglue.elapsed.get();
             (int)newSize.width != w || (int)newSize.height != h;) {
          // OSes sometimes do weird things. Set a time limit.
          if (uglue.elapsed.get() - t0 > 0.2) {
            logW("%d x %d vs %u x %u\n", w, h, newSize.width, newSize.height);
            ui.setUserFreeResize(true);
            break;
          }
          // Let the OS put this thread to sleep until the event comes.
          glfwWaitEventsTimeout(0.01 /*10ms*/);
          if (uglue.windowShouldClose()) {
            return 0;  // Already did cpool.deviceWaitIdle above.
          }
        }
      }

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
      if (doAppSwitching() || doLoadFromSave()) {
        return 1;
      }
    }
    return cpool.deviceWaitIdle();
  }
};

App* App::self{nullptr};

static int setupPresentMode(language::Instance& inst) {
  for (size_t i = 0; i < 1 && i < inst.devs.size(); i++) {
    auto& dev = *inst.devs.at(i);
    bool found = false;
    for (size_t j = 0; j < dev.presentModes.size(); j++) {
      if (dev.presentModes.at(j) == VK_PRESENT_MODE_FIFO_KHR) {
        found = true;
        dev.swapChainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
      }
    }
    if (!found) {
      for (size_t j = 0; j < dev.presentModes.size(); j++) {
        auto mode = dev.presentModes.at(j);
        logE("dev[%zu] avail %s %u\n", i, string_VkPresentModeKHR(mode),
             (unsigned)mode);
      }
      logE("dev[%zu] missing MODE_FIFO_KHR, %s\n", i,
           "Vulkan spec says it should always be present");
    }
  }
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
      setupPresentMode(inst) ||
      // inst.open() takes a while, especially if validation layers are on.
      inst.open({(uint32_t)width, (uint32_t)height})) {
    logE("inst.ctorError or inst.open failed\n");
  } else if (!inst.devs.size()) {
    logE("No vulkan devices found (or driver missing?)\n");
  } else {
    r = std::make_shared<App>(inst, window)->run();
  }
  return r;  // Destroy inst only after app.
}

static int crossPlatformMain(int argc, char** argv) {
#ifdef __ANDROID__
  androidSetNeedsMenuKey(1);  // For RetroGLFW.
#endif
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
  GLFWwindow* window = glfwCreateWindow(INIT_WIDTH, INIT_HEIGHT, "Retro",
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
