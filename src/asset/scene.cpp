/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 */

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "scene.h"

// scene.h #includes other glm headers
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/mat3x3.hpp>

namespace asset {

Frustum::Frustum(glm::mat4 view) : view(view) {
  viewInv = glm::inverse(view);
  for (int x = -1; x < 2; x += 2) {
    for (int y = -1; y < 2; y += 2) {
      for (int z = 0; z < 2; z++) {
        glm::vec4 p = viewInv * glm::vec4(x, y, z, 1.f);
        invpt.push_back(glm::vec3(p) * (1.f / p.w));
      }
    }
  }
  for (int axis = 1; axis < 8; axis <<= 1) {
    int a1 = (axis << 1) % 7;
    int a2 = (a1 << 1) % 7;
    for (int s = 0; s < axis*2; s += axis) {
      auto n = glm::cross(invpt[s + a1] - invpt[s], invpt[s + a2] - invpt[s]);
      plane.push_back(glm::vec4(n, -glm::dot(invpt[s], n)));
      // exchange a1 and a2 in place
      a1 ^= a2;
      a2 ^= a1;
      a1 ^= a2;
    }
  }
}

static inline glm::vec3 divideByW(glm::vec4 r) {
  return glm::vec3(r) * (1.f / r.w);
}

bool Frustum::isVisible(glm::vec4 p) {
  glm::vec3 r = divideByW(view * p);
  return fabsf(r.x) <= 1.f && fabsf(r.y) <= 1.f && r.z <= 1.f && r.z >= 0.f;
}

bool Frustum::isVisible(AABB box) {
  for (size_t i = 0; i < plane.size(); i++) {
    float test = -1.f;  // if test stays below 0, plane[i] rejects all corners
    for (int x = 0; x < 2; x++) {
      for (int y = 0; y < 2; y++) {
        for (int z = 0; z < 2; z++) {
          test = std::max(test, glm::dot(plane.at(i),
              glm::vec4(box[x].x, box[y].y, box[z].z, 1.f)));  // test a corner
        }
      }
    }
    if (test < 0.f) {
      return false;
    }
  }
  return true;
}

glm::mat4 Camera::perspective(float fovRadians, language::Device& dev) {
  auto& ex = dev.swapChainInfo.imageExtent;
  float h = ex.height;
  float horizonDistance = std::min(h * fabsf(pos.y) * .03125f + 32.f,
                                   1000.f);
  return glm::perspectiveLH_ZO(fovRadians, ex.width / h, 1.f, horizonDistance);
}

int FullscreenQuad::ctorError() {
  if (pipe.pipe) {
    // shaders.add() calls pipe.info() intentionally to add pipe to pass.
    logE("FullscreenQuad::ctorError: shaders.add() before ctorError()?\n");
    logE("FullscreenQuad::ctorError: pipe was already added, subpass lost.\n");
    return 1;
  }
  subpassI = pipe.pass.pipelines.size();
  if (!subpassI) {
    // A Fullscreen Quad visible "behind everything" is actually more efficient
    // drawn last! Early depth test can save on "overdraw." Is there any reason
    // to render a fullscreen quad first?
    logE("FullscreenQuad::ctorError: no prev pipe, alpha blend with what?\n");
    return 1;
  }
  // calling info() will add pipe to pass, so now pass.pipelines.size() is +1
  auto& pinfo = pipe.info();
  pinfo.dynamicStates.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
  pinfo.dynamicStates.emplace_back(VK_DYNAMIC_STATE_SCISSOR);
  pinfo.depthsci.depthTestEnable = VK_FALSE;
  pinfo.depthsci.depthWriteEnable = VK_FALSE;

  if (pipe.alphaBlendWith(pipe.pass.pipelines.at(subpassI - 1)->info,
                          VK_OBJECT_TYPE_PIPELINE) ||
      pipe.setName("fullscreenquad.pipe") ||
      pipe.pipe->pipelineLayout.setName("fullscreenquad.pipe layout")) {
    logE("FullscreenQuad::pipe.alphaBlendWith(prevPipe) failed\n");
    return 1;
  }
  return 0;
}

int FullscreenQuad::draw(language::Framebuf& framebuf,
                         command::CommandBuffer& cmd, size_t frameI) {
  uint32_t dsCount = 0;
  VkDescriptorSet* dsPtr = nullptr;
  if (!descriptorSet.empty()) {
    if (frameI >= descriptorSet.size()) {
      logE("FullscreenQuad::draw: frameI %zu vs. descriptorSet.size %zu\n",
           frameI, descriptorSet.size());
      return 1;
    }
    // Bind descriptorSet if it has been set up.
    dsCount = 1;
    dsPtr = &descriptorSet.at(frameI)->vk;
  }

  if (cmd.beginSubpass(pipe.pass, framebuf, (uint32_t)subpassI)) {
    logE("FullscreenQuad::draw: beginSubpass failed\n");
    return 1;
  }
  if (!show) {
    return 0;  // end subpass immediately without drawing anything.
  }

  auto& ex = pipe.pass.vk.dev.swapChainInfo.imageExtent;
  auto& pinfo = pipe.info();
  VkViewport& view = pinfo.viewports.at(0);
  view.width = float(ex.width);
  view.height = float(ex.height);
  auto& scis = pinfo.scissors.at(0);
  scis.extent = ex;

  if (cmd.bindGraphicsPipelineAndDescriptors(*pipe.pipe, 0, dsCount, dsPtr) ||
      cmd.setViewport(0, 1, &view) || cmd.setScissor(0, 1, &scis) ||
      cmd.draw(3, 1, 0, 0)) {
    logE("FullscreenQuad::draw: bindGraphicsPipelineAndDescriptors failed\n");
    return 1;
  }
  return 0;
}

}  // namespace asset
