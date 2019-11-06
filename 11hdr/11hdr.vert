// Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs.
out gl_PerVertex {
  vec4 gl_Position;
};
layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec3 fragViewVec;
layout(location = 3) out vec3 fragLightVec;
layout(location = 4) out vec2 fragTexCoord;

// Specify inputs that are constant ("uniform") for all vertices.
// The uniform buffer is updated by the app once per frame.
layout(binding = 0) uniform UniformBufferObject {
  mat4 modelview;
  mat4 proj;
  vec4 lightPos;
  float cubeDebug;
  float turn;
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
    gl_Position = ubo.proj * modelPos;
  }
  fragColor = inColor;
  fragLightVec = rot * ubo.lightPos.xyz - modelPos.xyz;
  fragTexCoord = inTexCoord;
}
