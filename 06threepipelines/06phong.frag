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
  float ambient = 0.6;
  vec3 N = normalize(fragNormal);
  vec3 L = normalize(fragLightVec);
  vec3 V = normalize(fragViewVec);
  vec3 R = reflect(-L, N);
  float diffuse = max(dot(N, L), 0.0) * 1.5;
  float specular = pow(max(dot(R, V), 0.0), 30) * 0.4;
  outColor = vec4(fragColor * (ambient + diffuse) + vec3(specular), 1.0);
}
