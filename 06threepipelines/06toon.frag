// Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs. location = 0 references the index in
// VkSubpassDescription.pcolorAttachments[].
layout (location = 0) out vec4 outColor;

// Specify inputs.
layout (location = 0) in vec3 fragNormal;
layout (location = 1) in vec3 fragColor;
layout (location = 2) in vec3 fragViewVec;
layout (location = 3) in vec3 fragLightVec;

void main() {
  // I is total illumination of the fragment.
  float I = dot(fragNormal, normalize(fragLightVec));

  // Y is brightness using the step function. This might be optimized as:
  // 0.8 + dot(vec2(1.2, 1.0), step(0.1, vec2(I, I - 0.3)))
  float Y = 0.8 + step(-0.2, I) * 1.2 + step(0.1, I);

  outColor = vec4(fragColor * Y, 1.0);
}
