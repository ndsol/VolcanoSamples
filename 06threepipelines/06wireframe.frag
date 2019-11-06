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
  outColor = vec4(fragColor, 1);
}
