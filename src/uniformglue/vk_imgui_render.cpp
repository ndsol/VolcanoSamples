/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This file contains a vulkan pipeline to render Dear ImGui.
 */
#include "imgui.h"
#include "uniformglue.h"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/mat2x2.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

typedef unsigned int uint;
#include "src/uniformglue/struct_imgui.vert.h"

static constexpr size_t VtxSize = sizeof(struct st_imgui_vert);
static constexpr size_t IdxSize = sizeof(ImDrawIdx);

void UniformGlue::getImGuiRotation(glm::mat2& rot, glm::vec2& translate) {
  switch (app.cpool.vk.dev.swapChainInfo.preTransform) {
    case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:
    case VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR:
      rot = glm::mat2(1.f, 0, 0, 1.f);
      break;
    case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
      rot = glm::mat2(0, -1.f, 1.f, 0);
      break;
    case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
      rot = glm::mat2(-1.f, 0, 0, -1.f);
      break;
    case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
      rot = glm::mat2(0, 1.f, -1.f, 0);
      break;
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_BIT_KHR:
      rot = glm::mat2(-1.f, 0, 0, 1.f);
      break;
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR:
      rot = glm::mat2(0, 1.f, 1.f, 0);
      break;
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_180_BIT_KHR:
      rot = glm::mat2(1.f, 0, 0, -1.f);
      break;
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR:
      rot = glm::mat2(0, -1.f, -1.f, 0);
      break;
    case VK_SURFACE_TRANSFORM_FLAG_BITS_MAX_ENUM_KHR:
      logF("VK_SURFACE_TRANSFORM_FLAG_BITS_MAX_ENUM_KHR is never used.\n");
      break;
  }
  auto& imageExtent = app.cpool.vk.dev.swapChainInfo.imageExtent;
  translate.x = (rot[0].x < 0 || rot[0].y < 0) ? imageExtent.width : 0;
  translate.y = (rot[1].x < 0 || rot[1].y < 0) ? imageExtent.height : 0;
}

int UniformGlue::imGuiInnerRender(struct ImDrawData* drawData,
                                  VkDrawIndexedIndirectCommand& indir) {
  // Check before writing vertex/index data that it will fit.
  VkDeviceSize want =
      drawData->TotalVtxCount * VtxSize + drawData->TotalIdxCount * IdxSize;
  if ((size_t)want > imGuiMaxBufUse) {
    logE("imGuiRender(%u): ImGui vtx/idx buf use %zu exceeds\n", nextImage,
         (size_t)want);
    logE("imGuiRender(%u): buf size %zu - not caught by checkImGuiBufSize?\n",
         nextImage, imGuiMaxBufUse);
    return 1;
  }
  auto& imageExtent = app.cpool.vk.dev.swapChainInfo.imageExtent;

  // DisplayPos + DisplaySize give the ImGui rectangle and coordinate system
  // for drawData.
  //
  // It should never be outside the values given for io.DisplaySize, but check.
  ImVec2 p1 = drawData->DisplayPos;
  if (p1.x < 0 || p1.y < 0 || p1.x >= imageExtent.width ||
      p1.y >= imageExtent.height) {
    logE("imGuiRender(%u): DisplayPos %fx%f, %fx%f not in (0,0)-(%u,%u)\n",
         nextImage, drawData->DisplayPos.x, drawData->DisplayPos.y,
         drawData->FramebufferScale.x, drawData->FramebufferScale.y,
         imageExtent.width, imageExtent.height);
    return 1;
  }
  if (drawData->DisplaySize.x > imageExtent.width ||
      drawData->DisplaySize.y > imageExtent.height) {
    logE("imGuiRender(%u): DisplayPos + Size %f %f past (%u,%u)\n", nextImage,
         drawData->DisplaySize.x, drawData->DisplaySize.y, imageExtent.width,
         imageExtent.height);
    return 1;
  }
  glm::mat2 rot;
  glm::vec2 translate;
  getImGuiRotation(rot, translate);
  rot[0].x *= drawData->FramebufferScale.x;
  rot[0].y *= drawData->FramebufferScale.y;
  rot[1].x *= drawData->FramebufferScale.x;
  rot[1].y *= drawData->FramebufferScale.y;

  memory::DescriptorSet* curDSet = &*imGuiDSet;

  auto* vtxBase = reinterpret_cast<struct st_imgui_vert*>(
      reinterpret_cast<char*>(imGuiBufMmap) + sizeOfIndir +
      imGuiMaxBufUse * nextImage);
  auto* idxBase =
      reinterpret_cast<ImDrawIdx*>(vtxBase + drawData->TotalVtxCount);
  auto* pVtx = vtxBase;
  auto* pIdx = idxBase;

  // Combine ImDrawCmd's by adding idxOfs to the set of indices in the
  // ImDrawCmd. This corresponds to appending the vertices to pVtx.
  ImDrawIdx idxOfs = 0;

  // Write vertex and index buffers.
  for (int i = 0; i < drawData->CmdListsCount; i++) {
    auto* c = drawData->CmdLists[i];
    for (auto j = 0; j < c->VtxBuffer.Size; j++) {
      ImDrawVert v = c->VtxBuffer.Data[j];
      pVtx->inColor = v.col;
      pVtx->inPosition =
          glm::vec2(v.pos.x - p1.x, v.pos.y - p1.y) * rot + translate;
      pVtx->inTexCoord = glm::vec2(v.uv.x, v.uv.y);
      pVtx->inClipRect = glm::vec4(0, 0, 0, 0);
      pVtx++;
    }

    unsigned kofs = 0;
    for (int j = 0; j < c->CmdBuffer.Size; j++) {
      const ImDrawCmd* b = &c->CmdBuffer[j];
      if (!b->TextureId) {
        continue;
      }

      // Disallow custom textures (an ImGui feature). Your app would need to
      // create a new DescriptorSet from imGuiDLib and write the Sampler into
      // the appropriate descriptor (descriptor->write and
      // cmdBuffer.bindDescriptorSets).
      if (b->TextureId != curDSet) {
        logE("imGui.bindDescriptorSets not implemented\n");
        return 1;
      }

      if (b->UserCallback) {
        b->UserCallback(c, b);
        continue;
      }
      auto clip1 = glm::vec2(b->ClipRect.x - p1.x, b->ClipRect.y - p1.y) * rot;
      auto clip2 = glm::vec2(b->ClipRect.z - p1.x, b->ClipRect.w - p1.y) * rot;
      for (unsigned k = 0; k < b->ElemCount; k++) {
        if (k + kofs >= (unsigned)c->IdxBuffer.Size) {
          logE("l%d b%d ElemCount=%u at k=%u + %u overran IdxBuffer %d\n", i, j,
               b->ElemCount, k, kofs, c->IdxBuffer.Size);
          break;
        }
        *pIdx = c->IdxBuffer.Data[k + kofs] + idxOfs;
        auto* q = &vtxBase[*pIdx];
        if (q >= pVtx) {
          logE("l%d b%d ElemCount=%u at k=%u + %u overran vertices (%zu)\n", i,
               j, b->ElemCount, k, kofs, (size_t)(pVtx - vtxBase));
          break;
        }
        if (clip1.x > clip2.x) {
          float t = clip1.x;
          clip1.x = clip2.x;
          clip2.x = t;
        }
        if (clip1.y > clip2.y) {
          float t = clip1.y;
          clip1.y = clip2.y;
          clip2.y = t;
        }
        q->inClipRect = glm::vec4(clip1.x + translate.x, clip1.y + translate.y,
                                  clip2.x + translate.x, clip2.y + translate.y);
        pIdx++;
      }
      kofs += b->ElemCount;
    }

    if (idxOfs + c->VtxBuffer.Size < idxOfs) {
      logE("index buffer overflow: idxOfs=%zu + Vtx size %zu\n", (size_t)idxOfs,
           (size_t)c->VtxBuffer.Size);
      return 1;
    }
    idxOfs += c->VtxBuffer.Size;
  }
  if ((size_t)(reinterpret_cast<char*>(pIdx) -
               reinterpret_cast<char*>(vtxBase)) > imGuiMaxBufUse) {
    logE(
        "index buffer overflowed imGuiMaxBufUse - recompile with larger "
        "buffer\n");
    return 1;
  }

  // Store draw command.
  indir.indexCount = pIdx - idxBase;
  indir.instanceCount = 1;  // No instancing is being done.
  // Calculate offset to index buffer and vertex buffer.
  indir.firstIndex = (drawData->TotalVtxCount * VtxSize) / IdxSize;
  indir.vertexOffset = 0;
  indir.firstInstance = 0;  // No instancing is being done.
  return 0;
}

int UniformGlue::imGuiRender(struct ImDrawData* drawData) {
  if (!imGuiBufMmap) {
    logE("imGuiRender(%u): imGuiBufMmap not mapped\n", nextImage);
    return 1;
  }
  if (nextImage * sizeOfOneIndir > sizeOfIndir) {
    logE("imGuiRender: invalid nextImage %u\n", nextImage);
    return 1;
  }

  char* pIndir = reinterpret_cast<char*>(imGuiBufMmap);
  pIndir += nextImage * sizeOfOneIndir;
  // If any error causes imGuiInnerRender to bail, ensure no drawing is done.
  memset(pIndir, 0, sizeOfOneIndir);

  int r = imGuiInnerRender(
      drawData, *reinterpret_cast<VkDrawIndexedIndirectCommand*>(pIndir));

#ifdef VOLCANO_DISABLE_VULKANMEMORYALLOCATOR
  VkMappedMemoryRange VkInit(range);
  range.offset = 0;
  range.size = VK_WHOLE_SIZE;
  if (imGuiBuf.mem.flush(std::vector<VkMappedMemoryRange>{range})) {
#else  /*VOLCANO_DISABLE_VULKANMEMORYALLOCATOR*/
  if (imGuiBuf.mem.flush()) {
#endif /*VOLCANO_DISABLE_VULKANMEMORYALLOCATOR*/
    logE("imGuiRender(%u): imGuiBuf.mem.flush failed\n", nextImage);
    return 1;
  }
  return r;
}
