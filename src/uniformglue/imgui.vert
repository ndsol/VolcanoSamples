// Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs.
out gl_PerVertex {
  vec4 gl_Position;
};
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec4 fragColor;
layout(location = 3) out flat vec4 fragClipRect;

// Inputs have same meaning as struct ImDrawVert in "imgui.h".
// inColor is packed as R8G8B8A8 in a uint (32 bits).
// clipRect is a workaround where clipping is done in frag shader instead
// of Vulkan drawing command to update clip rect there. It allows clipping
// to be encoded in an indirect draw command.
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in uint inColor;
layout(location = 3) in vec4 inClipRect;

layout(push_constant) uniform PushConsts {
  vec2 ortho;
} pushConsts;

void main() {
  vec2 p = inPosition * pushConsts.ortho + vec2(-1.0);
  gl_Position = vec4(p, 0.0, 1.0);
  fragTexCoord = inTexCoord;

  // Unpack inColor
  fragColor = vec4(inColor & 0xff,
                   (inColor >> 8) & 0xff,
                   (inColor >> 16) & 0xff,
                   inColor >> 24) / 255.0;

  fragClipRect = inClipRect;
}
