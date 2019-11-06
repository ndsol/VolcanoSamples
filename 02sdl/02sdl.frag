// Copyright (c) 2017 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs. location = 0 references the index in
// VkSubpassDescription.pcolorAttachments[].
layout(location = 0) out vec4 outColor;

// Specify inputs. Must match vertex shader.
layout(location = 0) in vec4 fragColor;

void main() {
  outColor = fragColor;
}
