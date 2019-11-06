// Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Specify outputs. location = 0 references the index in
// VkSubpassDescription.pcolorAttachments[].
layout(location = 0) out vec4 outColor;

// OUT_W is the size in texels of the output texture (cubemap)
layout(constant_id = 0) const float OUT_W = 0;
// OUT_H is the size in texels of the output texture (cubemap)
layout(constant_id = 1) const float OUT_H = 0;
// SAMPLER_W is the size in texels of the input texture (equirectangular)
layout(constant_id = 2) const float SAMPLER_W = 0;
// SAMPLER_H is the size in texels of the input texture (equirectangular)
layout(constant_id = 3) const float SAMPLER_H = 0;
// SIDES is true to render the 4 sides of the cube, false to render top/bottom
layout(constant_id = 4) const bool SIDES = false;

// Specify inputs. Must match vertex shader.
layout(location = 0) in vec2 fragTexCoord;

// A sampler2D is declared "uniform" because the entire texture is accessible
// (read-only) in each invocation of the fragment shader.
layout(binding = 0) uniform sampler2D texSampler;

// Constant definitions:

const float invPi = 1./3.141592653589;

// There are 4 sides. They are all rendered into the same output image.
const float sides = 4.;

// toElAzi converts eye to spherical coordinates.
vec2 toElAzi(vec3 eye) {
  // Convert from rectangular (eye) to azimuth and elevation (spherical
  // coordinates). The equirectangular projection or spherical panorama is
  // then texture(texSampler, vec2(azimuth, elevation)). In other words, the
  // texture is a rectangle pretending to be a spherical coordinate system.
  // (Hence the name "equi" "rectangular" projection.)
  //
  // NOTE: This divides by pi since the texture coords are in the range
  //       (0,0) - (1,1), i.e. the math uses all integers times pi.
  // NOTE: Divide longitude by 2 since eye.x has a range of 2 from -1 to 1.
  return vec2(atan(-eye.z, eye.x) / 2, acos(eye.y / length(eye))) * invPi;
}

vec4 renderSides(vec4 eye) {
  vec2 p0 = toElAzi(eye.xyz);
  // Re-insert eye.w, i.e. assumed eye was side[1] before, now re-add 90 deg
  // for each side beyond side[1].
  return texture(texSampler, vec2(p0.x + eye.w / (sides * 2), p0.y));
}

vec4 renderTopBottom(vec4 eye) {
  vec2 p0 = toElAzi(eye.xzy);  // eye.xzy swizzle: point eye to the top face.

  // Approximate an arc from p0 along the longitudinal and latitudinal axes.
  // This is a line segment, which will be slightly less than the length of a
  // true arc.
  vec2 step = 1. / vec2(SAMPLER_W, SAMPLER_H);

  // Sample polar coordinates from p0 to p0 + .5 / vec2(OUT_W, OUT_H) along a
  // a diagonal.
  bool bottom = fragTexCoord.x > 0;
  vec2 b = .5 / vec2(OUT_W, OUT_H);
  float lodBias = clamp(length(b) / length(step), 0.5, 1) * -4;
  b += p0;
  uint n = 0;  // Limit the total number of samples.
  vec4 sum = vec4(0);
  for (; (p0.x < b.x && p0.y < b.y && n < 4096) || n < 1; p0 += step, n++) {
    // Map the rendered top to 2 images starting at x=-1 and x=-0.5. Map
    // bottom to 2 images starting at x=0 and x=0.5.
    vec2 uv = p0;
    if (bottom) {
      // Adjust p to sample bottom face's texels.
      uv = vec2(1) - uv;
    }
    uv.x = -uv.x;
    sum += texture(texSampler, uv, lodBias);
  }
  return vec4(sum.rgb / n, 1);
}

void main() {
  // fragTexCoord: x in (-1, 1). y in (-1, 1) for one of the 4 sides of a cube.
  // side[0] is -X face. side[1] is +Z. side[2] is +X. side[3] is -Z.
  // Or if SIDES == false, side[0] is top, side[1] is another copy of top,
  // side[2] is bottom, side[3] is another copy of bottom.
  //
  // https://stackoverflow.com/questions/34250742
  //
  // Render the output with 4 images horizontally laid out:
  // side[0] x in (-1, -0.5) side[1] x in (-0.5, 0) ... side[3] x in (0.5, 1)
  //
  // Latitude is in the range pi/4 to 3*pi/4 for the 4 sides. The top face is
  // latitudes from 0 to pi/4. The bottom face latitudes are from 3*pi/4 to pi.
  //
  // Find an eye-to-texel vector pointing at fragTexCoord, assuming this is
  // side[1]. side[1] +Z face projects fragTexCoord.x onto eye.x and
  // fragTexCoord.y onto eye.y.
  //
  // eye.xz only used for length(eye), so it doesn't matter which side.
  float x = (fragTexCoord.x + 1) * sides;
  float m = mod(x, 2) - 1;
  vec4 eye = vec4(m, fragTexCoord.y, 1, x - m - 3);
  outColor = SIDES ? renderSides(eye) : renderTopBottom(eye);
}
