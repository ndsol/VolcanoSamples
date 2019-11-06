// Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs. location = 0 references the index in
// VkSubpassDescription.pcolorAttachments[].
layout(location = 0) out vec4 outColor;

// Specify inputs that are constant ("uniform") for all fragments.
// A sampler2D is declared "uniform" because the entire texture is accessible
// (read-only) in each invocation of the fragment shader.
layout(binding = 1) uniform sampler2D texSampler;

// Specify inputs.
layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;

void main() {
  if (fragTexCoord.x < -0.5 && fragTexCoord.y < -0.5) {
    // texturing disabled
    outColor = vec4(fragColor, 1);
  } else {
    outColor = vec4(texture(texSampler, fragTexCoord).xyz, 1);
  }
}
