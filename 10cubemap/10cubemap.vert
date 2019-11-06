// Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs.
out gl_PerVertex {
  vec4 gl_Position;
};
layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out vec3 fragViewVec;
// The last component of fragLightVec is set to 'slider'.
// Packing components together like this reduces register pressure for the GPU.
layout(location = 3) out vec4 fragLightVec;
layout(location = 4) out vec2 fragTexCoord;

// Specify inputs that are constant ("uniform") for all vertices.
// The uniform buffer is updated by the app once per frame.
layout(binding = 0) uniform UniformBufferObject {
  mat4 modelview;
  mat4 invModelView;
  mat4 proj;
  vec4 lightPos;
  float treeReflect;
  float grassReflect;
  float rockReflect;
  float dirtReflect;
} ubo;

// Specify inputs that vary per vertex (read from the vertex buffer).
// gl_VertexIndex is still defined as the vertex index (0 .. N).
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

void main() {
  mat3 rot = mat3(ubo.modelview);
  vec4 modelPos = ubo.modelview * vec4(inPosition, 1.0);
  float slider = 0.0;
  float a = 1.0;
  if (inNormal.x == 0 && inNormal.y == 0 && inNormal.z == 0) {
    // Use this vertex to draw the skybox. Ignore ubo.proj.
    fragNormal = inNormal;
    vec3 pos = inPosition;
    fragViewVec = pos;
    pos = rot * pos;
    pos.x = -pos.x;
    gl_Position = pos.xyzz;  // Force w == z, "move this fragment to back"
  } else {
    // This is a normal vertex. Use ubo.proj.
    fragNormal = normalize(rot * inNormal);
    fragViewVec = -modelPos.xyz;
    vec4 pos = ubo.proj * modelPos;

    vec3 C = inColor;
    if (C.r > 6.4175e-02 && C.r < 6.4177e-02 && C.g > 7.8069e-02 &&
        C.g < 7.8071e-02 && C.b > 3.4987e-02 && C.b < 3.4989e-02) {
      slider = ubo.grassReflect;
    } else if (C.r > 6.4009e-02 && C.r < 6.4010e-02 && C.g > 6.4009e-02 &&
               C.g < 6.4010e-02 && C.b > 6.4009e-02 && C.b < 6.4010e-02) {
      slider = ubo.rockReflect;
    } else if (C.r > 6.6824e-02 && C.r < 6.6825e-02 && C.g > 4.2910e-02 &&
               C.g < 4.2911e-02 && C.b > 3.6293e-02 && C.b < 3.6294e-02) {
      slider = ubo.dirtReflect;
    } else if (C.r > 5.7843e-01 && C.r < 5.7844e-01 && C.g > 3.8169e-01 &&
               C.g < 3.8170e-01 && C.b > 1.4839e-01 && C.b < 1.4841e-01) {
      // Gold chest
    } else if (C.r > 1.0992e-01 && C.r < 1.0994e-01 && C.g > 1.6633e-01 &&
               C.g < 1.6634e-01 && C.b > 6.2003e-02 && C.b < 6.2004e-02) {
      // Bright green grass blades
    } else if (C.r > 6.1263e-02 && C.r < 6.1264e-02 && C.g > 2.8092e-02 &&
               C.g < 2.8093e-02 && C.b > 1.9444e-02 && C.b < 1.9445e-02) {
      slider = ubo.treeReflect;
    }
    if (slider >= 0.0) {
      // In 10cubemap.frag, slider controls how much reflectColor is added.
    } else {
      // if slider is negative, it reduces a (alpha)
      a = max(a + slider, 0.0);
      // Drawing transparent fragments has weird rendering artifacts. Need a full
      // Order-Independent Transparency shader to get it right. This semi-works
      // because a very simple model is being drawn. Still has weird artifacts.
      if (a < 0.99) {    // If transparent, force w == z, "move this frag to back"
        pos = pos.xyzz;  // for depth test: only output this frag if no solid frag
        // Mobile GPUs' depth precision is small: fix Z fighting with cubemap:
        pos.w *= 1.00001;
      }
    }
    gl_Position = pos;
  }

  fragColor = vec4(inColor, a);
  // Pack 'slider' in with fragLightVec:
  fragLightVec = vec4(rot * ubo.lightPos.xyz - modelPos.xyz, slider);
  fragTexCoord = inTexCoord;
}
