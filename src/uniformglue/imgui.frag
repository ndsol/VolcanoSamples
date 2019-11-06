// Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs.
layout(location = 0) out vec4 outColor;

// Specify inputs.
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec4 fragColor;
layout(location = 3) in flat vec4 fragClipRect;

layout(binding = 0) uniform sampler2D fontSampler;

void main() {
  if (gl_FragCoord.x < fragClipRect.x ||
      gl_FragCoord.y < fragClipRect.y ||
      gl_FragCoord.x > fragClipRect.z ||
      gl_FragCoord.y > fragClipRect.w) {
    outColor = vec4(0);
  } else {
    outColor = fragColor * texture(fontSampler, fragTexCoord);
  }
}
