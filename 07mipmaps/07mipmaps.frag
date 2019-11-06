// Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs. location = 0 references the index in
// VkSubpassDescription.pcolorAttachments[].
layout(location = 0) out vec4 outColor;

// Specify inputs.
layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in flat int debugOneLod;

// Specify inputs that are constant ("uniform") for all fragments.
// A sampler2D is declared "uniform" because the entire texture is accessible
// (read-only) in each invocation of the fragment shader.
layout(binding = 1) uniform sampler2D texSampler;

#ifdef METAL_NO_TEXTUREQUERYLOD
// The METAL_NO_TEXTUREQUERYLOD macro is a made-up macro used for 07mipmaps:
// see BUILD.gn where extra_flags = [ "-DMETAL_NO_TEXTUREQUERYLOD" ] defines it.
//
// But Metal doesn't support textureQueryLod. This is a workaround:
// https://stackoverflow.com/questions/24388346/how-to-access-automatic-mipmap-level-in-glsl-fragment-shader-texture
// Does not handle GL_TEXTURE_MIN_LOD/GL_TEXTURE_MAX_LOD/GL_TEXTURE_LOD_BIAS
// nor implementation-specific differences allowed by the OpenGL spec.
//
// tex is the sampler2D to query
// texCoord is the texel coordinate in normalized units, i.e. 0-1
vec2 workaroundTextureQueryLod(sampler2D tex, in vec2 texCoord) {
  float n = textureQueryLevels(tex) - 1;
  vec2 c = texCoord * textureSize(tex, 0);  // convert to texels
  vec2 dx_vtc = dFdx(c);
  vec2 dy_vtc = dFdy(c);
  vec2 delta = 0.5 * log2(vec2(dot(dy_vtc, dy_vtc), dot(dx_vtc, dx_vtc)));
  return vec2(min(max(0, delta.x), n), min(max(0, delta.y), n));
}

#define textureQueryLod workaroundTextureQueryLod
#endif

void main() {
  // See for more information:
  // https://stackoverflow.com/questions/24388346/how-to-access-automatic-mipmap-level-in-glsl-fragment-shader-texture
  float n = textureQueryLod(texSampler, fragTexCoord).x;
  vec4 origColor = texture(texSampler, fragTexCoord);
  outColor = mix(vec4(1,0,0,1), origColor, clamp(abs(n - debugOneLod), 0, 1));
}
