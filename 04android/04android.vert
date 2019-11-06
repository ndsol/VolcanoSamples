// Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs.
out gl_PerVertex {
  vec4 gl_Position;
};
layout(location = 0) out vec4 fragColor;

// Specify inputs that vary per vertex (read from the vertex buffer).
// gl_VertexIndex is still defined as the vertex index (0 .. N).
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;

void main() {
  gl_Position = vec4(inPosition, 0, 1);
  fragColor = vec4(inColor, 1);
}
