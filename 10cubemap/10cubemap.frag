// Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs. location = 0 references the index in
// VkSubpassDescription.pcolorAttachments[].
layout(location = 0) out vec4 outColor;

// Specify inputs that are constant ("uniform") for all vertices.
// The uniform buffer is updated by the app once per frame.
layout(binding = 0) uniform UniformBufferObject {
  mat4 modelview;
  mat4 invModelView;
  mat4 proj;
  vec4 lightPos;
  float treeReflect;
  float grassReflect;
  float rockReflect;
  float dirtReflect;
} ubo;

// A samplerCube is declared "uniform" because the entire texture is accessible
// (read-only) in each invocation of the fragment shader.
layout(binding = 1) uniform samplerCube cubemap;

// Specify inputs.
layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec3 fragViewVec;
// The last component of fragLightVec is 'slider'.
layout(location = 3) in vec4 fragLightVec;
layout(location = 4) in vec2 fragTexCoord;

vec3 phongShading(vec3 N, vec3 L, vec3 V) {
  float ambient = 0.6;
  vec3 R = reflect(-L, N);

  float diffuse = max(dot(N, L), 0.0) * 1.5;
  float specular = pow(max(dot(R, V), 0.0), 30) * 0.4;
  return fragColor.rgb * (ambient + diffuse) + vec3(specular);
}

vec3 toonShading(vec3 L) {
  // I is total illumination of the fragment.
  float I = dot(fragNormal, L);

  // Y is brightness using the step function. This might be optimized as:
  // 0.8 + dot(vec2(1.2, 1.0), step(0.1, vec2(I, I - 0.3)))
  float Y = 0.8 + step(-0.2, I) * 1.2 + step(0.1, I);

  return fragColor.rgb * Y;
}

vec3 reflective(vec3 N, vec3 L, vec3 V) {
  float ambient = 0.5;
  vec3 C = fragColor.rgb;
  vec3 R = reflect(-L, N);
  float specular = pow(max(dot(R, V), 0.0), 16.0) * 0.4;
  vec3 reflected = (ubo.invModelView * vec4(reflect(V, N), 0.0)).xyz;
  vec3 reflectColor = texture(cubemap, reflected).rgb;
  float slider = fragLightVec.w;
  if (slider >= 0.0) {
    reflectColor *= slider;
    specular *= slider;
  } else {
    reflectColor = vec3(0);
    specular = 0;
  }
  reflectColor += C;

  float diffuse = max(dot(N, L), 0.0);
  return reflectColor * (ambient + diffuse) + vec3(specular);
}

void main() {
  if (fragNormal.x == 0 && fragNormal.y == 0 && fragNormal.z == 0) {
    outColor = vec4(texture(cubemap, fragViewVec).rgb, 1);
  } else {
    vec3 N = normalize(fragNormal);
    vec3 L = normalize(fragLightVec.xyz);
    vec3 V = normalize(fragViewVec);
    outColor = vec4(reflective(N, L, V), fragColor.w);
  }
}
