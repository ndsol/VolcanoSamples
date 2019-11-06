// Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs.
out gl_PerVertex {
  vec4 gl_Position;
};
layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec3 fragViewVec;
layout(location = 3) out vec3 fragLightVec;

// Specify inputs that are constant ("uniform") for all vertices.
// The uniform buffer is updated by the app once per frame.
layout(binding = 0) uniform UniformBufferObject {
  mat4 proj;
  mat4 view;
  vec4 lightPos;  // only xyz used, w ignored
} ubo;

// Specify inputs that vary per vertex (read from the vertex buffer).
// gl_VertexIndex is still defined as the vertex index (0 .. N).
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

// Specify inputs that vary per instance (read from a second vertex buffer, but
// for clarity, call it the "instance buffer").
// Compare to the other vertex shader which uses UniformBufferObject.
// NOTE: For some odd reason, GLSL defines these this way, instead of
// layout(binding = 1, location = 3) in vec4 loc; - if you add "binding = 1", it
// errors out with "'binding' : requires uniform or buffer storage qualifier".
//
// But these are bound to binding 1, not binding 0, in the C++ code.
layout(location = 3) in vec4 loc;
layout(location = 4) in vec4 rot;

vec3 qrotate(vec4 rot, vec3 v){ 
  return v + 2.0 * cross(cross(v, rot.xyz) + rot.w * v, rot.xyz);
}

mat3 q_to_mat3(vec4 rot) {
  vec3 identity = vec3(1.0, 0.0, 0.0);
  return mat3(qrotate(rot, identity),
              qrotate(rot, identity.zxy),
              qrotate(rot, identity.yzx));
}

void main() {
  mat3 modelview = q_to_mat3(rot);
  vec3 pos = modelview * inPosition + loc.xyz;
  gl_Position = ubo.proj * ubo.view * vec4(pos, 1.0);
  modelview = mat3(ubo.view) * modelview;
  fragNormal = normalize(modelview * inNormal);
  fragColor = inColor;
  fragViewVec = -pos;
  fragLightVec = (ubo.view * vec4(ubo.lightPos.xyz - pos, 1)).xyz;
}
