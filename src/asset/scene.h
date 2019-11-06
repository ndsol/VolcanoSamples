/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 */

#include <src/memory/memory.h>
#include <src/science/science.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec2.hpp>

#include "asset.h"

#pragma once

namespace asset {

typedef struct LightSpot {
  glm::vec3 pos;
} LightSpot;

typedef struct Camera {
  // pos is kept out of the view matrix:
  // the view matrix by itself does not translate objects in world space
  // this is to avoid floating point precision errors with large pos values
  glm::vec3 pos;

  glm::mat4 view;
  glm::mat4 proj;
  glm::quat orient;

  // perspective calculates a perspective projection matrix given a fov
  // and dev is used to get dev.swapChainInfo.imageExtent.
  glm::mat4 perspective(float fovRadians, language::Device& dev);

  inline glm::mat4 viewFromOrient() {
    return glm::mat4_cast(orient);
  }
} Camera;

typedef struct AABB {
  glm::vec3 bound[2]{{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}};

  glm::vec3& operator[](size_t i) {
    if (i < sizeof(bound) / sizeof(bound[0])) {
      return bound[i];
    }
    logF("AABB: [%zu] out of bounds\n", i);
    return bound[0];
  }

  const glm::vec3& operator[](size_t i) const {
    return operator[](i);
  }

  void extend(glm::vec3 p) {
    bound[0].x = std::min(bound[0].x, p.x);
    bound[0].y = std::min(bound[0].y, p.y);
    bound[0].z = std::min(bound[0].z, p.z);
    bound[1].x = std::max(bound[1].x, p.x);
    bound[1].y = std::max(bound[1].y, p.y);
    bound[1].z = std::max(bound[1].z, p.z);
  }
} AABB;

// Frustum takes a glm::mat4 (presumably camera.proj * camera.view) and inverts
// it to define pt and plane in the world which would clip to the frustum in
// camera space.
typedef struct Frustum {
  Frustum(glm::mat4 view);
  const glm::mat4 view;
  glm::mat4 viewInv;
  std::vector<glm::vec3> invpt;
  std::vector<glm::vec4> plane;

  // isVisible returns true if p is visible in camera space.
  bool isVisible(glm::vec4 p);
  bool isVisible(glm::vec3 p) {
    return isVisible(glm::vec4(p, 1.f));
  }

  // isVisible returns true if box is visible in camera space.
  bool isVisible(AABB box);
} Frustum;

typedef struct Scene {
  std::vector<std::shared_ptr<Library>> lib;
  std::vector<Camera> camera;
  std::vector<LightSpot> lightSpot;
} Scene;

// FullscreenQuad takes a vertex and fragment shader and renders a single
// triangle. The vertex shader must have no inputs, and probably should
// contain the following GLSL code at a bare minimum:
//
// vec2 vert[3] = vec2[]( vec2(-1, -1), vec2(-1, 3), vec2(3, -1) );
// out gl_PerVertex {
//   vec4 gl_Position;
// };
// layout(location = 0) out vec2 fragTexCoord;
// void main() {
//   gl_Position = vec4(vert[gl_VertexIndex], 0, 1);
//   fragTexCoord = 0.5 * gl_Position.xy + 0.5;
// }
//
// NOTE: The name FullscreenQuad is deceptive. This only draws 1 triangle.
typedef struct FullscreenQuad {
  FullscreenQuad(command::RenderPass& pass)
      : shaders{pass.vk.dev}, descriptorLibrary{pass.vk.dev}, pipe{pass} {}

  // show can be set to false, and then rebuild the command buffers (which
  // should call draw() below). It will convert the render pass to a no-op.
  bool show{true};
  // shaders is *not* set up by ctorError due to a chicken and egg situation:
  // if your app wants to add a shader or two, ctorError will initialize pipe
  // for you. Great!
  //
  // But before ctorError can call shaders.finalizeDescriptorLibrary, your app
  // needs to add the shaders! And if you call shaders.add(pipe, shader) before
  // ctorError, then ctorError will *not* work and will exit with an error.
  //
  // Thus shaders.finalizeDescriptorLibrary is *not* done. Your app can do that
  // and must do that before using descriptorLibrary below.
  //
  // If your shaders do not have any descriptor set inputs (you could use only
  // push constants or shader specialization for example) -- then call
  // shaders.add(pipe, shader) just to attach the shader to the pipe, and
  // that's all you need.
  science::ShaderLibrary shaders;
  // descriptorLibrary is built using shaders.finalizeDescriptorLibrary().
  science::DescriptorLibrary descriptorLibrary;

  // descriptorSet must be resized and filled in after ctorError with
  // DescriptorSet objects obtained from descriptorLibrary.makeSet().
  // If descriptorSet is empty, draw() will assume there are none. Otherwise,
  // there must be one per framebuf.
  std::vector<std::shared_ptr<memory::DescriptorSet>> descriptorSet;
  science::PipeBuilder pipe;
  size_t subpassI{0};

  WARN_UNUSED_RESULT int ctorError();
  WARN_UNUSED_RESULT int draw(language::Framebuf& framebuf,
                              command::CommandBuffer& cmd, size_t frameI);
} FullscreenQuad;

}  // namespace asset
