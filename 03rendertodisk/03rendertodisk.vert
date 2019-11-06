// Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs.
out gl_PerVertex {
  vec4 gl_Position;
};
layout(location = 0) out vec4 fragColor;

vec4 positions[3] = vec4[](
  vec4( 0.5, -0.5, 0, 1),
  vec4(-0.5,  0.5, 0, 1),
  vec4( 0.5,  0.5, 0, 1)
);

vec4 colors[3] = vec4[](
  vec4(1.0, 0.0, 0.0, 1),
  vec4(0.0, 1.0, 0.0, 1),
  vec4(0.0, 0.0, 1.0, 1)
);

void main() {
  gl_Position = positions[gl_VertexIndex];
  fragColor = colors[gl_VertexIndex];
}
