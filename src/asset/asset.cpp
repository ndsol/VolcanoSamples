/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 */

#include "asset.h"

#define _USE_MATH_DEFINES /*Windows otherwise hides M_PI*/
#include <math.h>
#include <src/core/VkPtr.h>

namespace asset {

int BaseAsset::doTri(indicesType i0, indicesType i1, indicesType i2,
                     std::vector<Vertex>& raw, std::vector<indicesType>& shared,
                     VertexIndex& out, uint32_t flags) {
  if (raw.size() <= i0 || raw.size() <= i1 || raw.size() <= i2) {
    logE("doTri: raw.size=%zu i0=%zu i1=%zu i2=%zu\n", raw.size(), (size_t)i0,
         (size_t)i1, (size_t)i2);
    return 1;
  }

  auto N = glm::cross(raw[i1].P - raw[i0].P, raw[i2].P - raw[i1].P);
  auto len = glm::length(N);
  // N is not normalized so len is 2 * the area of the tri: skip degenerate tri
  if (len < glm::epsilon<decltype(len)>()) {
    return 0;
  }
  N /= len;

  std::vector<Vertex> r;
  r.reserve(3);
  r.push_back(raw[i0]);
  r.back().N = N;
  r.push_back(raw[i1]);
  r.back().N = N;
  r.push_back(raw[i2]);
  r.back().N = N;
  if (eval.first) {
    if (eval.first(eval.second, r, flags)) {
      return 1;
    }
  }
  if (flags & VERT_PER_FACE) {
    // Emit the face now, do not average normals and colors.
    out.order.emplace_back(out.vert.size());
    out.vert.emplace_back(r[0]);
    out.order.emplace_back(out.vert.size());
    out.vert.emplace_back(r[1]);
    out.order.emplace_back(out.vert.size());
    out.vert.emplace_back(r[2]);
    return 0;
  }

  raw[i0].add(r[0]);
  raw[i1].add(r[1]);
  raw[i2].add(r[2]);
  shared.push_back(i0);
  shared.push_back(i1);
  shared.push_back(i2);
  return 0;
}

int BaseAsset::doShared(std::vector<Vertex>& raw,
                        std::vector<indicesType>& shared, VertexIndex& out) {
  size_t indexBase = out.vert.size();  // The VERT_PER_FACE vertices are first.
  for (size_t i = 0; i < raw.size(); i++) {
    auto len = glm::length(raw.at(i).N);
    if (len < glm::epsilon<decltype(len)>()) {
      // delete raw.at(i) by decrementing each j in shared where j >= i
      // This can happen if all faces on this vertex set VERT_PER_FACE.
      for (size_t j = 0; j < shared.size(); j++) {
        if (shared.at(j) >= i) {
          shared.at(j)--;
        }
      }
    } else {
      raw.at(i).divideByScalar(len);
      out.vert.push_back(raw.at(i));
    }
  }
  out.order.reserve(out.order.size() + shared.size());
  for (size_t i = 0; i < shared.size(); i++) {
    out.order.emplace_back(shared.at(i) + indexBase);
  }
  return 0;
}

int Revolv::toVertices(VertexIndex& out) {
  if (rots < 3 + rotStart || aspectZ < 0.f || pt.size() < 2) {
    logE("invalid: rots=%zu rotStart=%zu aspectZ=%e pt.size=%zu\n",
         (size_t)rots, (size_t)rotStart, aspectZ, pt.size());
    return 1;
  }

  // Compute the raw point cloud
  std::vector<Vertex> raw;
  const uint32_t angles = rots - rotStart;
  for (size_t i = 0; i < angles; i++) {
    float a = (2 * M_PI * (i + rotStart)) / rots;
    for (size_t j = 0; j < pt.size(); j++) {
      raw.emplace_back();
      Vertex& v = raw.back();
      v.P.x = pt.at(j).x * cos(a);
      v.P.y = pt.at(j).y;
      v.P.z = pt.at(j).x * sin(a) * aspectZ;
    }
  }

  // For simplicity, think of pt as a straight line from (1, -1) to (1, 1),
  // so this is going to sweep out a cylinder volume.
  //
  // Each face around the cylinder is a rectangle (4 vertices).
  // The end caps are a triangle fan (3 vertices per face).
  //
  // Each vertex is used 3 times. If pt were made of more line segments, the
  // vertices in the middle of the cylinder would only be used 2 times.
  //
  // Generate top and bottom caps
  std::vector<indicesType> shared;
  size_t anglesXpts = angles * pt.size();
  if (!(flags & DELETE_CAP_B)) {
    size_t cap1 = pt.size();
    for (size_t i = pt.size() * 2; i < anglesXpts; cap1 = i, i += pt.size()) {
      if (doTri(0, cap1, i, raw, shared, out, 0)) {
        logE("Revolv: eval cap[%zu+%d] failed\n", i, 0);
        return 1;
      }
    }
  }
  if (!(flags & DELETE_CAP_T)) {
    size_t cap1 = pt.size() * 2 - 1;
    for (size_t i = cap1 + pt.size(); i < anglesXpts;
         cap1 = i, i += pt.size()) {
      // Top: reverse order of i, cap1 to remain counter-clockwise.
      if (doTri(pt.size() - 1, i, cap1, raw, shared, out, 0)) {
        logE("Revolv: eval cap[%zu+%d] failed\n", i, 1);
        return 1;
      }
    }
  }
  // FIXME: if rotStart != 0, generate left and right caps.

  // Generate sides
  for (size_t i = 0; i < angles; i++) {
    for (size_t j = 0; j < pt.size() - 1; j++) {
      indicesType p0 = i * pt.size() + j;
      indicesType p2 = ((i + 1) % angles) * pt.size() + j;
      if (doTri(p0, p0 + 1, p2 + 1, raw, shared, out, 0)) {
        logE("Revolv: eval face[%zu+%d] failed\n", i, 0);
        return 1;
      }
      if (doTri(p0, p2 + 1, p2, raw, shared, out, 0)) {
        logE("Revolv: eval face[%zu+%d] failed\n", i, 1);
        return 1;
      }
    }
  }
  return doShared(raw, shared, out);
}

static glm::vec3 heightMapVec3(HeightMapPoint& pt, size_t x, size_t z,
                               float scaleX, float aspect) {
  // TODO: xz displacement
  return glm::vec3(x * scaleX, pt.y, z * scaleX * aspect);
}

int HeightMap::toVertices(VertexIndex& out) {
  if (!pt) {
    logE("invalid: HeightMap::pt is null\n");
    return 1;
  }
  size_t height = pt->size() / width;
  if (width < 2 || height < 2 || pt->size() % width) {
    logE("invalid: width=%zu height=%zu, pt->size mod width = %zu\n",
         width, height, pt->size() % width);
    return 1;
  }

  HeightMapPoint* data = pt->data();
  std::vector<indicesType> shared;
  std::vector<Vertex> raw;
  raw.reserve(width * height);
  // Write a row for Z = 0
  for (size_t j = 0; j < width; j++, data++) {
    raw.emplace_back();
    raw.back().P = heightMapVec3(*data, j, 0, scaleX, aspect);
  }
  for (size_t i = 1; i < height; i++) {
    raw.emplace_back();
    raw.back().P = heightMapVec3(*data, 0, i, scaleX, aspect);
    data++;
    for (size_t j = 1; j < width; j++, data++) {
      indicesType p3 = raw.size();  // Grab raw.size() before emplace_back()
      raw.emplace_back();
      raw.back().P = heightMapVec3(*data, j, i, scaleX, aspect);
      indicesType p1 = p3 - width;
      if (doTri(p1 - 1, p3, p1, raw, shared, out,
                data[0].flags & VERT_PER_FACE)) {
        logE("HeightMap: eval [%zu,%zu,%d] failed\n", j, i, 0);
        return 1;
      }
      if (doTri(p1 - 1, p3 - 1, p3, raw, shared, out,
                data[-1].flags & VERT_PER_FACE)) {
        logE("HeightMap: eval [%zu,%zu,%d] failed\n", j, i, 1);
        return 1;
      }
    }
  }
  return doShared(raw, shared, out);
}

}  // namespace asset
