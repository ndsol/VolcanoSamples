// Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs.
out gl_PerVertex {
  vec4 gl_Position;
};
layout(location = 0) out vec2 fragTexCoord;

// Specify inputs that are constant ("uniform") for all vertices.
layout(binding = 0) uniform UniformBufferObject {
  mat4 modelview;
  mat4 proj;
  vec4 lightPos;
  float cubeDebug;
  float turn;
} ubo;

vec4 positions[4] = vec4[](
  vec4( 1,  1, 0, 1),
  vec4( 1, -1, 0, 1),
  vec4(-1,  1, 0, 1),
  vec4(-1, -1, 0, 1)
);

vec2 uvCoords[4] = vec2[](
  vec2(1.0,-1.0),
  vec2(1.0, 1.0),
  vec2(-1.0,-1.0),
  vec2(-1.0, 1.0)
);

void main() {
  gl_Position = positions[gl_VertexIndex];
  fragTexCoord = uvCoords[gl_VertexIndex];
}
