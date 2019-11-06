// Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs. location = 0 references the index in
// VkSubpassDescription.pcolorAttachments[].
layout(location = 0) out vec4 outColor;

// Specify specialization constants. The SPIR-V code is optimized and any
// unused paths are eliminated. This allows one "uber shader" to be
// specialized as needed.
layout(constant_id = 0) const int SHADING_MODE = 0;

// Specify inputs that are constant ("uniform") for all fragments.
// A sampler2D is declared "uniform" because the entire texture is accessible
// (read-only) in each invocation of the fragment shader.
layout(binding = 1) uniform sampler2D texSampler;

// Specify inputs.
layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec3 fragViewVec;
layout(location = 3) in vec3 fragLightVec;
layout(location = 4) in vec2 fragTexCoord;

vec4 phongShading() {
  float ambient = 0.6;
  vec3 N = normalize(fragNormal);
  vec3 L = normalize(fragLightVec);
  vec3 V = normalize(fragViewVec);
  vec3 R = reflect(-L, N);
  float diffuse = max(dot(N, L), 0.0) * 1.5;
  float specular = pow(max(dot(R, V), 0.0), 30) * 0.4;
  return vec4(fragColor * (ambient + diffuse) + vec3(specular), 1.0);
}

vec4 toonShading() {
  // I is total illumination of the fragment.
  float I = dot(fragNormal, normalize(fragLightVec));

  // Y is brightness using the step function. This might be optimized as:
  // 0.8 + dot(vec2(1.2, 1.0), step(0.1, vec2(I, I - 0.3)))
  float Y = 0.8 + step(-0.2, I) * 1.2 + step(0.1, I);

  return vec4(fragColor * Y, 1.0);
}

vec4 wireFrameShading() {
  return vec4(fragColor, 1);
}

vec4 texturedShading() {
  float adj = 6.0f;  // The model's baked-in fragColor is very dark. Adjust it.
  return texture(texSampler, fragTexCoord) * vec4(fragColor, 1) * adj;
}

void main() {
  switch (SHADING_MODE) {
  case 0:
    outColor = phongShading();
    break;
  case 1:
    outColor = toonShading();
    break;
  case 2:
    outColor = wireFrameShading();
    break;
  case 3:
    outColor = texturedShading();
    break;
  default:
    // If SHADING_MODE is something unexpected, this will help with debugging.
    outColor = vec4(0.5,0.5,0.5,1);
    break;
  }
}
