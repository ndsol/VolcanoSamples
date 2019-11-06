/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This file contains a vulkan pipeline to render Dear ImGui.
 */
#include <stdlib.h>

#include <algorithm>
#include <chrono>

#include "imgui.h"
#include "uniformglue.h"

typedef unsigned int uint;
#include "src/uniformglue/imgui.frag.h"
#include "src/uniformglue/imgui.vert.h"
#include "src/uniformglue/struct_imgui.vert.h"

namespace frag {
#include "src/uniformglue/struct_imgui.frag.h"
}

static constexpr size_t VtxSize = sizeof(struct st_imgui_vert);
static constexpr size_t IdxSize = sizeof(ImDrawIdx);

int UniformGlue::imGuiInit() {
  if (app.pass.vk) {
    logE("imGuiInit: too late, render pass already created.\n");
    logE("imGuiInit: see method documentation in uniform_glue.h\n");
    return 1;
  }
  imGuiWanted = true;
  return 0;
}

int UniformGlue::buildPassAndTriggerResize() {
  if (app.pass.pipelines.empty() || !app.pass.pipelines.back()) {
    logE("%s can only be called after creating at least one pipeline.\n",
         "buildPassAndTriggerResize");
    return 1;
  }
  if (app.pass.vk) {
    // internalImguiInit may need to add a pipeline. Also, this method calls
    // onResized for you which will call pass.ctorError if !app.pass.vk
    logE("%s - RenderPass already built. Did you call onResized already?\n",
         "buildPassAndTriggerResize");
    return 1;
  }
  glfwSetWindowUserPointer(window, this);
  glfwSetWindowSizeCallback(window, onGLFWResized);
  glfwSetWindowRefreshCallback(window, onGLFWRefresh);
  glfwSetWindowFocusCallback(window, onGLFWFocus);
  glfwSetCursorEnterCallback(window, onGLFWcursorEnter);
  glfwGetWindowContentScale(window, &scaleX, &scaleY);
#if defined(__linux__) && !defined(__ANDROID__)
  scaleX = 1.f;  // Not all monitors report correctly on Desktop Linux.
  scaleY = 1.f;
#endif
  glfwSetWindowContentScaleCallback(window, onGLFWcontentScale);
#if defined(GLFW_HAS_MULTITOUCH) && !defined(VOLCANO_TEST_NO_MULTITOUCH)
  glfwSetMultitouchEventCallback(window, onGLFWmultitouch);
#else  /*GLFW_HAS_MULTITOUCH*/
  glfwSetCursorPosCallback(window, onGLFWcursorPos);
  glfwSetMouseButtonCallback(window, onGLFWmouseButtons);
  glfwSetScrollCallback(window, onGLFWscroll);
#endif /*GLFW_HAS_MULTITOUCH*/
  glfwSetKeyCallback(window, onGLFWkey);
  glfwSetCharCallback(window, onGLFWchar);
  glfwSetMonitorCallback(onGLFWmonitorChange);

  auto qfam_i = app.cpool.vk.dev.getQfamI(language::PRESENT);
  if (qfam_i == (decltype(qfam_i))(-1)) {
    logE("dev.getQfamI(%d) failed\n", language::PRESENT);
    return 1;
  }
  auto& qfam = app.cpool.vk.dev.qfams.at(qfam_i);
  if (qfam.queues.size() < 1) {
    logE("BUG: queue family PRESENT with %zu queues\n", qfam.queues.size());
    return 1;
  }
  presentQueue = qfam.queues.at(memory::ASSUME_PRESENT_QINDEX);

  return internalImguiInit() ||
         shaders.finalizeDescriptorLibrary(descriptorLibrary) ||
         app.onResized(app.cpool.vk.dev.swapChainInfo.imageExtent,
                       memory::ASSUME_POOL_QINDEX);
}

static void DearImGuiAndroidSetClipboardText(void* userData, const char* text) {
  glfwSetClipboardString((GLFWwindow*)userData, text);
}

static const char* DearImGuiAndroidGetClipboardText(void* userData) {
  return glfwGetClipboardString((GLFWwindow*)userData);
}

int UniformGlue::internalImguiInit() {
  if (!imGuiWanted) {
    return 0;
  }

  imguiContext = ImGui::CreateContext();
  auto& io = ImGui::GetIO();
  io.UserData = nullptr;  // Not used.
  io.IniFilename = "invalid\\path\\stops/file/from/being/created.ini";
#ifndef IMGUI_DISABLE_OBSOLETE_FUNCTIONS
  io.RenderDrawListsFn = nullptr;  // Not used.
#endif
  // Set io.DisplayFramebufferScale to the initial value.
  onGLFWcontentScale(window, scaleX, scaleY);

  // Add any requested fonts.
  for (std::shared_ptr<ImFontConfig> p : fonts) {
    if (!p) {
      // Ignore shared_ptr if it is empty.
      continue;
    }
    if (!p->FontData || !p->FontDataSize) {
      io.Fonts->AddFontDefault(&(*p));
    } else {
      io.Fonts->AddFont(&(*p));
    }
  }

  io.SetClipboardTextFn = DearImGuiAndroidSetClipboardText;
  io.GetClipboardTextFn = DearImGuiAndroidGetClipboardText;
  io.ClipboardUserData = window;
  imGuiTimestamp = Timer::now();

  io.KeyMap[ImGuiKey_Tab] = GLFW_KEY_TAB;
  io.KeyMap[ImGuiKey_LeftArrow] = GLFW_KEY_LEFT;
  io.KeyMap[ImGuiKey_RightArrow] = GLFW_KEY_RIGHT;
  io.KeyMap[ImGuiKey_UpArrow] = GLFW_KEY_UP;
  io.KeyMap[ImGuiKey_DownArrow] = GLFW_KEY_DOWN;
  io.KeyMap[ImGuiKey_PageUp] = GLFW_KEY_PAGE_UP;
  io.KeyMap[ImGuiKey_PageDown] = GLFW_KEY_PAGE_DOWN;
  io.KeyMap[ImGuiKey_Home] = GLFW_KEY_HOME;
  io.KeyMap[ImGuiKey_End] = GLFW_KEY_END;
  io.KeyMap[ImGuiKey_Insert] = GLFW_KEY_INSERT;
  io.KeyMap[ImGuiKey_Delete] = GLFW_KEY_DELETE;
  io.KeyMap[ImGuiKey_Backspace] = GLFW_KEY_BACKSPACE;
  io.KeyMap[ImGuiKey_Space] = GLFW_KEY_SPACE;
  io.KeyMap[ImGuiKey_Enter] = GLFW_KEY_ENTER;
  io.KeyMap[ImGuiKey_Escape] = GLFW_KEY_ESCAPE;
  io.KeyMap[ImGuiKey_A] = GLFW_KEY_A;
  io.KeyMap[ImGuiKey_C] = GLFW_KEY_C;
  io.KeyMap[ImGuiKey_V] = GLFW_KEY_V;
  io.KeyMap[ImGuiKey_X] = GLFW_KEY_X;
  io.KeyMap[ImGuiKey_Y] = GLFW_KEY_Y;
  io.KeyMap[ImGuiKey_Z] = GLFW_KEY_Z;
#ifdef _WIN32
  io.ImeWindowHandle = glfwGetWin32Window(window);
#endif

  if (imGuiPipe) {
    logE("imGuiInit: why is imGuiPipe not null at top?\n");
    return 1;
  }
  imGuiPipe = std::make_shared<science::PipeBuilder>(app.pass);

  if (VtxSize <= sizeof(ImDrawVert)) {
    // struct st_imgui_vert is dynamically created by shader reflection from
    // the vertex shader. It must have ImDrawVert fields plus some more.
    logE("imGuiInit: ImDrawVert %zu size, %zu too small\n", sizeof(ImDrawVert),
         VtxSize);
    return 1;
  }
  if (IdxSize != sizeof(uint16_t)) {
    logE("imGuiInit: ImDrawIdx size wrong for cmdBuffer.bindIndexBuffer\n");
    return 1;
  }

  auto vert = std::make_shared<command::Shader>(app.cpool.vk.dev);
  auto frag = std::make_shared<command::Shader>(app.cpool.vk.dev);
  if (vert->loadSPV(spv_imgui_vert, sizeof(spv_imgui_vert)) ||
      frag->loadSPV(spv_imgui_frag, sizeof(spv_imgui_frag)) ||
      shaders.add(*imGuiPipe, vert, imGuiLayoutIndex) ||
      shaders.add(*imGuiPipe, frag, imGuiLayoutIndex)) {
    logE("imGuiShaders failed\n");
    return 1;
  }

  auto& pipeInfo = imGuiPipe->info();
  pipeInfo.rastersci.cullMode = VK_CULL_MODE_NONE;
  pipeInfo.dynamicStates.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
  pipeInfo.dynamicStates.emplace_back(VK_DYNAMIC_STATE_SCISSOR);
  if (imGuiPipe->addVertexInput<st_imgui_vert>() ||
      imGuiPipe->setName("imGuiPipe") ||
      imGuiPipe->pipe->pipelineLayout.setName("imGuiPipe layout") ||
      vert->setName("imgui.vert") || frag->setName("imgui.frag")) {
    logE("imGuiPipe->addVertexInput or setName failed\n");
    return 1;
  }

  // Check that imGuiPipe is the last in pass.pipelines
  if (app.pass.pipelines.size() < 2) {
    logE("endRenderPass(): imGuiPipe not added? got %zu\n",
         app.pass.pipelines.size());
    return 1;
  }

  auto& prevPipe = *app.pass.pipelines.at(app.pass.pipelines.size() - 2);
  if (imGuiPipe->alphaBlendWith(prevPipe.info, VK_OBJECT_TYPE_PIPELINE)) {
    logE("imGuiPipe->alphaBlendWith failed\n");
    return 1;
  }

  // Do NOT depth test and discard fragments or write to the depth buffer.
  // alphaBlendWith() auto-enabled these, and it breaks hit testing.
  pipeInfo.depthsci.depthTestEnable = VK_FALSE;
  pipeInfo.depthsci.depthWriteEnable = VK_FALSE;

  unsigned char* pixels;
  int width, height;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  imGuiFontSampler.info.magFilter = VK_FILTER_LINEAR;
  imGuiFontSampler.info.minFilter = VK_FILTER_LINEAR;
  imGuiFontSampler.image->info.format = VK_FORMAT_R8G8B8A8_UNORM;
  imGuiFontSampler.image->info.extent = {(uint32_t)width, (uint32_t)height, 1};
  VkDeviceSize rowStride = width * 4 /*format R8G8B8A8 is 4 bytes-per-texel*/;
  if (stage.mmapMax() < rowStride) {
    logE("imGuiFontSampler: stage.mmapMax()=%zu but rowStride=%zu\n",
         stage.mmapMax(), (size_t)rowStride);
    return 1;
  }

  if (imGuiFontSampler.setName("imGuiFontSampler") ||
      imGuiFontSampler.imageView.setName("imGuiFontSampler.imageView") ||
      imGuiFontSampler.image->setName("imGuiFontSampler.image") ||
      imGuiFontSampler.ctorError()) {
    logE("imGuiFontSampler.ctorError or setName failed\n");
    return 1;
  }

  // Copy the bytes to imGuiFontSampler in chunks.
  VkDeviceSize total = height * rowStride;
  VkDeviceSize ofs = 0;
  while (ofs < total) {
    VkDeviceSize chunk = stage.mmapMax();
    if (chunk > total - ofs) {
      chunk = total - ofs;
    }
    // Round down chunk to the next smallest rowStride.
    VkDeviceSize rows = chunk / rowStride;
    chunk = rows * rowStride;
    if (!chunk) {
      logE("imGuiFontSampler: ofs=%zu BUG: chunk=0\n", (size_t)ofs);
      return 1;
    }

    std::shared_ptr<memory::Flight> flight;
    if (stage.mmap(*imGuiFontSampler.image, chunk, flight)) {
      logE("imGuiFontSampler: ofs=%zu stage.mmap failed\n", (size_t)ofs);
      return 1;
    }
    memcpy(flight->mmap(), pixels + ofs, chunk);

    flight->copies.resize(1);
    VkBufferImageCopy& copy = flight->copies.at(0);
    copy.bufferOffset = 0;
    copy.bufferRowLength = imGuiFontSampler.image->info.extent.width;
    copy.imageSubresource = imGuiFontSampler.image->getSubresourceLayers(0);
    copy.imageOffset = {0, static_cast<int32_t>(ofs / rowStride), 0};
    copy.imageExtent = imGuiFontSampler.image->info.extent;
    copy.bufferImageHeight = rows;
    copy.imageExtent.height = rows;

    if (stage.flushAndWait(flight)) {
      logE("imGuiFontSampler: ofs=%zu stage.flushAndWait failed\n",
           (size_t)ofs);
      return 1;
    }

    ofs += chunk;
  }
  // Transition the image layout of imGuiFontSampler.
  science::SmartCommandBuffer smart(stage.pool, stage.poolQindex);
  if (smart.ctorError() || smart.autoSubmit() ||
      smart.barrier(*imGuiFontSampler.image,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
    logE("barrier(SHADER_READ_ONLY) failed\n");
    return 1;
  }
  return 0;
}

int UniformGlue::endRenderPass(command::CommandBuffer& cmdBuffer,
                               size_t framebuf_i) {
  if (isImguiAvailable()) {
    if (!imGuiPipe) {
      logE("endRenderPass(): imGuiInit did not create imGuiPipe.\n");
      return 1;
    }
    if (framebuf_i >= sizeOfIndir / sizeOfOneIndir) {
      logE("endRenderPass(): BUG: need more than %zu sizeOfIndir\n",
           size_t(sizeOfIndir / sizeOfOneIndir));
      return 1;
    }
    if (!descriptorLibrary.isFinalized()) {
      logE("endRenderPass: must finalizeDescriptorLibrary before onResized\n");
      return 1;
    }

    uint32_t subpass = app.pass.pipelines.size() - 1;
    if (app.pass.pipelines.at(subpass) != imGuiPipe->pipe) {
      // This is to make Dear ImGui convenient to enable and disable. It is
      // always just "tacked on the end." Maybe you called things in the
      // wrong order?
      logE("endRenderPass(): imGuiPipe must be last in pipelines.\n");
      logE("endRenderPass(): Did you call buildPassAndTriggerResize()?\n");
      logE("endRenderPass(): Do not call app.onResized() directly.\n");
      return 1;
    }
    if (imGuiAddCommands(cmdBuffer, framebuf_i, subpass)) {
      logE("UniformGlue::imGuiAddCommands failed\n");
      return 1;
    }
  }
  if (cmdBuffer.endRenderPass() || cmdBuffer.end()) {
    logE("cmdBuffer.endRenderPass failed\n");
    return 1;
  }
  return 0;
}

int UniformGlue::imGuiAddCommands(command::CommandBuffer& cb, size_t framebuf_i,
                                  uint32_t subpass) {
  auto& io = ImGui::GetIO();
  auto& dev = app.cpool.vk.dev;
  if (!imGuiDSet) {
    imGuiDSet = descriptorLibrary.makeSet(imGuiDSetIndex, imGuiLayoutIndex);
    if (!imGuiDSet) {
      logE("endRenderPass: imGuiDSet makeSet failed\n");
      return 1;
    }
    if (imGuiDSet->setName("imGuiDSet") ||
        imGuiDSet->write(frag::bindingIndexOffontSampler(),
                         std::vector<science::Sampler*>{&imGuiFontSampler})) {
      logE("imGuiFontSampler or imGuiDSet failed\n");
      return 1;
    }
    io.Fonts->TexID = (void*)&*imGuiDSet;
  }

  if (!imGuiBuf.vk) {
    // imGuiBuf is made big enough to contain vertex and index data for
    // several frames at once.
    //
    // At offset 0, imGuiBuf reserves sizeOfIndir for as many
    // VkDrawIndexedIndirectCommand as might be needed.
    // dev.availableFeatures.multiDrawIndirect not used.
    //
    // This formula also converts imGuiBufWant from arbitrary units to bytes.
    VkDeviceSize want = sizeOfIndir + imGuiBufWant * (VtxSize * 5 + IdxSize);

    // Enforce nonCoherentAtomSize by rounding up.
    VkDeviceSize roundUp = dev.physProp.properties.limits.nonCoherentAtomSize;
    imGuiBuf.info.size =
        ((want * dev.framebufs.size() + roundUp - 1) / roundUp) * roundUp;
    if (imGuiBuf.info.size < want) {
      logE("BUG: imGuiBuf.info.size got %llu, want at least %llu\n",
           (unsigned long long)imGuiBuf.info.size, (unsigned long long)want);
      return 1;
    }
    imGuiBuf.info.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

    // Allocate imGuiBuf. This is not flexible - no resizing later.
    if (imGuiBuf.reset() || imGuiBuf.ctorAndBindHostVisible()) {
      logE("imGuiBuf.ctorAndBindHostVisible failed\n");
      return 1;
    }

    // Prepare VkDrawIndexedIndirectCommand with 0 values.
#ifdef VOLCANO_DISABLE_VULKANMEMORYALLOCATOR
    if (imGuiBuf.mem.mmap(&imGuiBufMmap, 0, imGuiBuf.info.size)) {
#else  /*VOLCANO_DISABLE_VULKANMEMORYALLOCATOR*/
    if (imGuiBuf.mem.mmap(&imGuiBufMmap)) {
#endif /*VOLCANO_DISABLE_VULKANMEMORYALLOCATOR*/
      logE("imGuiBuf.mem.mmap failed\n");
      return 1;
    }
    auto* pIndir =
        reinterpret_cast<VkDrawIndexedIndirectCommand*>(imGuiBufMmap);
    memset(pIndir, 0, sizeOfIndir);
#ifdef VOLCANO_DISABLE_VULKANMEMORYALLOCATOR
    VkMappedMemoryRange VkInit(range);
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    if (imGuiBuf.mem.flush(std::vector<VkMappedMemoryRange>{range})) {
#else  /*VOLCANO_DISABLE_VULKANMEMORYALLOCATOR*/
    if (imGuiBuf.mem.flush()) {
#endif /*VOLCANO_DISABLE_VULKANMEMORYALLOCATOR*/
      logE("imGuiBuf.mem.flush failed\n");
      return 1;
    }

    // Allocation simplification: each frame gets an equal share of imGuiBuf.
    imGuiMaxBufUse = (imGuiBuf.info.size - sizeOfIndir) / dev.framebufs.size();
    // Round each buffer's usage down to something it can actually use.
    if (VtxSize < IdxSize) {
      // Assume ImDrawVert is the largest struct that will be allocated.
      logE("BUG: sizeof(ImDrawVert) < sizeof(ImDrawIdx)\n");
      return 1;
    }
    size_t align = ((VtxSize + 31) & ~31);  // Align the start of each "share."
    imGuiMaxBufUse = (imGuiMaxBufUse / align) * align;
  }

  io.DisplaySize.x = dev.swapChainInfo.imageExtent.width;
  io.DisplaySize.y = dev.swapChainInfo.imageExtent.height;
  // bvk is an array of Vulkan handles, must have same size as vtxOfs.
  VkBuffer bvk[] = {imGuiBuf.vk};
  VkDeviceSize vtxOfs[] = {sizeOfIndir + framebuf_i * imGuiMaxBufUse};

  VkViewport& viewport = imGuiPipe->info().viewports.at(0);
  viewport.width = io.DisplaySize.x;
  viewport.height = io.DisplaySize.y;
  imGuiPipe->info().scissors.at(0).extent = dev.swapChainInfo.imageExtent;

  // Use Push Constants instead of a uniform buffer because the orthogonal
  // projection matrix for 2D rendering is just 2 floats.
  // * Push Constants are fast but can only change when the command buffer is
  //   rebuilt.
  // * Uniform Buffers can be changed without rebuilding the command buffer
  //
  // Why not just set the Viewport to some large fixed size, skip the Push
  // Constants, and just send up vertices as-is? That's a clever optimization,
  // but this is sample code. The extra confusion isn't worth it here. Also,
  // the GPU hardware culls things that are outside the range (-1,-1)-(1,1)
  // (i.e. normalized coordinates) after the Viewport scales all vertices to
  // normalized coordinates for culling.
  //
  // Using Push Constants the vertices are scaled to screen space after
  // viewport culling. The clip rectangle in the vertex buf is in screen space.
  // ImGui needs that clip rectangle for its rendering - which is heavy
  // "overdraw" as in, wasted fill rate, but simpler to understand.
  PushConsts push;
  push.ortho[0] = 2.0f / dev.swapChainInfo.imageExtent.width;
  push.ortho[1] = 2.0f / dev.swapChainInfo.imageExtent.height;
  if (cb.beginSubpass(app.pass, dev.framebufs.at(framebuf_i), subpass) ||
      cb.bindGraphicsPipelineAndDescriptors(*imGuiPipe->pipe, 0, 1,
                                            &imGuiDSet->vk) ||
      cb.setViewport(0, 1, &imGuiPipe->info().viewports.at(0)) ||
      cb.setScissor(0, 1, &imGuiPipe->info().scissors.at(0)) ||
      cb.pushConstants(*imGuiPipe->pipe, VK_SHADER_STAGE_VERTEX_BIT, push) ||
      cb.bindVertexBuffers(0, sizeof(bvk) / sizeof(bvk[0]), bvk, vtxOfs) ||
      cb.bindIndexBuffer(imGuiBuf.vk, vtxOfs[0],
                         // already checked in imGuiInit to be uint16_t
                         VK_INDEX_TYPE_UINT16) ||
      cb.drawIndexedIndirect(
          imGuiBuf.vk, framebuf_i * sizeOfOneIndir /* offset */,
          1 /* drawCount: (cannot be >1 without multiDrawIndirect) */)) {
    logE("imGuiAddCommands failed\n");
    return 1;
  }
  return 0;
}

int UniformGlue::checkImGuiBufSize(struct ImDrawData* d) {
  VkDeviceSize s = d->TotalVtxCount * VtxSize + d->TotalIdxCount * IdxSize;
  if ((size_t)s <= imGuiMaxBufUse) {
    return 0;
  }
  size_t largerWant = (s + VtxSize * 5 + IdxSize - 1) / (VtxSize * 5 + IdxSize);
  // Add extra padding to attempt to avoid lots of little reallocations.
  largerWant = (largerWant - imGuiBufWant) * 4 + imGuiBufWant;

  // Destroy imGuiBuf and imGuiBufMmap. Recreated in imGuiAddCommands.
  if (imGuiBufMmap) {
    imGuiBuf.mem.munmap();
    imGuiBufMmap = nullptr;
  }
  imGuiBufWant = largerWant;

  return 1;  // Tell UniformGlue::acquire rebuild is needed.
}
