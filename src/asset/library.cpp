/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 */

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "asset.h"

namespace asset {

static constexpr int dbg = 0;

int Library::ctorError(
    size_t vertexSize_, size_t instSize_, size_t maxIndices_,
    size_t bindingIndexOfInstanceBuf_) {
  if (vertexSize_ <= instSize_) {
    logE("Library::ctorError: vertexSize %zu instSize %zu\n",
         vertexSize_, instSize_);
    logE("Library::ctorError: instSize must be less than vertexSize\n");
    return 1;
  }
  vertexSize = vertexSize_ - instSize_;
  instSize = instSize_;
  bindingIndexOfInstanceBuf = bindingIndexOfInstanceBuf_;
  maxVertices = uglue.stage.mmapMax() / vertexSize;
  maxIndices = maxIndices_;
  order.reserve(maxIndices_);
  vertexBuf.info.size = vertexSize * maxVertices;
  indexBuf.info.size = sizeof(indicesType) * maxIndices;
  if (vertexBuf.reset() || vertexBuf.ctorAndBindDeviceLocal() ||
      indexBuf.reset() || indexBuf.ctorAndBindDeviceLocal() ||
      vertexBuf.setName("Library::vertexBuf") ||
      indexBuf.setName("Library::indexBuf")) {
    logE("Library::ctorError: vertexBuf.ctorAndBindDeviceLocal failed\n");
    return 1;
  }

  FreeBlock f;
  f.base = 0;
  f.use = maxVertices;
  freeV.push_front(f);
  f.use = maxIndices;
  freeO.push_front(f);
  if (dbg) logI("init free v0 + %zu o0 + %zu\n", maxVertices, maxIndices);
  return 0;
}

int Library::alloc(size_t& firstV, size_t& firstO, VertexIndex& batch,
                   VertexIndex& out, BaseAsset& a) {
  // Find a freeV. Still need to find a freeO.
  auto prevFv = freeV.end();
  for (auto fv = freeV.begin(); fv != freeV.end(); prevFv = fv++) {
    if (fv->use >= out.vert.size()) {
      // Now find a freeO.
      auto prevFo = freeO.end();
      for (auto fo = freeO.begin(); fo != freeO.end(); prevFo = fo++) {
        if (fo->use >= out.order.size()) {
          // Copy block location to firstV, firstO
          if (!batch.vert.empty()) {
            if (firstV >= fv->base) {
              logE("alloc: took %zu first, then %zu (they are not sorted!)\n",
                   firstV, fv->base);
              return 1;
            }
            if (firstO >= fo->base) {
              logE("alloc: took %zu first, then %zu (they are not sorted!)\n",
                   firstO, fo->base);
              return 1;
            }
            if (firstV + batch.vert.size() != fv->base) {
              // No split writes to vertex buffer (data the middle is on GPU,
              // no way to know what the contents are at this point.)
              // Instead, stay in state == ADDED until write() is called again.
              // NOTE: no infinite loop - already know !batch.vert.empty()
              return 0;
            }
            // Copy indices between firstO + batch.order.size() and g->order0.
            batch.order.insert(batch.order.end(),
                               order.begin() + (firstO + batch.order.size()),
                               order.begin() + fo->base);
          } else {
            firstV = fv->base;  // Set up the first use in the batch.
            firstO = fo->base;
          }
          a.vblk.base = fv->base;
          a.vblk.use = out.vert.size();
          a.oblk.base = fo->base;
          a.oblk.use = out.order.size();
          a.inst.cmd.indexCount = out.order.size();
          a.inst.cmd.instanceCount = 0;
          a.inst.cmd.firstIndex = fo->base;
          a.inst.cmd.vertexOffset = fv->base;
          a.inst.cmd.firstInstance = 0;

          // Update fv and fo, removing the allocated part.
          if (dbg) logI("alloc v%zu + %zu o%zu + %zu\n", fv->base, fv->use,
               fo->base, fo->use);
          size_t n = std::min(fv->use, out.vert.size());
          fv->base += n;
          fv->use -= n;
          if (!fv->use) {
            if (dbg) logI("  - del freeV\n");
            if (prevFv == freeV.end()) {
              freeV.pop_front();
            } else {
              freeV.erase_after(prevFv);
            }
          }
          else if (dbg) logI("  - now freeV %zu + %zu\n", fv->base, fv->use);
          // fv is invalid after this point.
          n = std::min(fo->use, out.order.size());
          fo->base += n;
          fo->use -= n;
          if (!fo->use) {
            if (dbg) logI("  - del freeO\n");
            if (prevFo == freeO.end()) {
              freeO.pop_front();
            } else {
              freeO.erase_after(prevFo);
            }
          }
          else if (dbg) logI("  - now freeO %zu + %zu\n", fo->base, fo->use);
          // fo is invalid after this point.

          batch.vert.insert(batch.vert.end(), out.vert.begin(),
                            out.vert.end());
          batch.order.insert(batch.order.end(), out.order.begin(),
                             out.order.end());
          totalVertUse += out.vert.size();
          totalOrderUse += out.order.size();
          a.state_ = ADD_WAIT;
          return 0;
        }
      }

      // Could not find a freeO.
      logE("write: %zu/%zu %s add %zu more\n",
           totalOrderUse + batch.order.size(), maxIndices,
           "indices used, cannot", out.order.size());
      logE("write: %zu/%zu %s add %zu more\n",
           totalVertUse + batch.vert.size(), maxVertices, "verts used,",
           out.vert.size());
      logE("write: out of memory. Maybe create another Library?\n");
      return 1;
    }
  }

  // Could not find a freeV.
  logE("write: %zu/%zu %s add %zu more\n",
       totalVertUse + batch.vert.size(), maxVertices, "verts used, cannot",
       out.vert.size());
  logE("write: %zu/%zu %s add %zu more\n",
       totalOrderUse + batch.order.size(), maxIndices, "indices used,",
       out.order.size());
  logE("write: out of memory. Maybe create another Library?\n");
  return 1;
}

int Library::free(BaseAsset& a) {
  // Add a.vblk back to freeV; a.oblk back to freeO
  auto prevFv = freeV.end();
  for (auto fv = freeV.begin(); fv != freeV.end(); prevFv = fv++) {
    if (fv->base + fv->use <= a.vblk.base) {
      continue;
    } else if ((fv->base <= a.vblk.base && fv->base + fv->use > a.vblk.base) ||
               (fv->base < a.vblk.base + a.vblk.use)) {
      logE("BUG: free(v=%zu + %zu) is already a freeblock %zu + %zu\n",
           a.vblk.base, a.vblk.use, fv->base, fv->use);
      return 1;
    }
    // a.vblk gets inserted before fv
    if (prevFv == freeV.end()) {
      if (a.vblk.base + a.vblk.use > fv->base) {
        logE("BUG: free(v=%zu + %zu) but head %zu + %zu\n", a.vblk.base,
             a.vblk.use, fv->base, fv->use);
        return 1;
      }
      if (dbg) logI("  - add freeV %zu + %zu before %zu + %zu at head\n", a.vblk.base, a.vblk.use, fv->base, fv->use);
      freeV.push_front(a.vblk);
      prevFv = freeV.begin();
    } else {
      if (dbg) logI("  - add freeV %zu + %zu after %zu + %zu\n", a.vblk.base, a.vblk.use, prevFv->base, prevFv->use);
      freeV.insert_after(prevFv, a.vblk);
    }
    // Merge blocks - should only ever merge up to 2 times
    fv = prevFv;
    for (fv++; fv != freeV.end() && prevFv->base + prevFv->use == fv->base; ) {
      if (dbg) logI("  - mergfreeV %zu + %zu with %zu + %zu\n", prevFv->base, prevFv->use, fv->base, fv->use);
      prevFv->use += fv->use;
      fv = freeV.erase_after(prevFv);
    }
    break;
  }
  auto prevFo = freeO.end();
  for (auto fo = freeO.begin(); fo != freeO.end(); prevFo = fo++) {
    if (fo->base + fo->use <= a.oblk.base) {
      continue;
    } else if ((fo->base <= a.oblk.base && fo->base + fo->use > a.oblk.base) ||
               (fo->base < a.oblk.base + a.oblk.use)) {
      logE("BUG: free(o=%zu + %zu) is already a freeblock %zu + %zu\n",
           a.oblk.base, a.oblk.use, fo->base, fo->use);
      return 1;
    }
    // a.oblk gets inserted before fo
    if (prevFo == freeO.end()) {
      if (a.oblk.base + a.oblk.use > fo->base) {
        logE("BUG: free(o=%zu + %zu) but head %zu + %zu\n", a.oblk.base,
             a.oblk.use, fo->base, fo->use);
        return 1;
      }
      if (dbg) logI("  - add freeO %zu + %zu before %zu + %zu at head\n", a.oblk.base, a.oblk.use, fo->base, fo->use);
      freeO.push_front(a.oblk);
      prevFo = freeO.begin();
    } else {
      if (dbg) logI("  - add freeO %zu + %zu after %zu + %zu\n", a.oblk.base, a.oblk.use, prevFo->base, prevFo->use);
      freeO.insert_after(prevFo, a.oblk);
    }
    // Merge blocks - should only ever merge up to 2 times
    fo = prevFo;
    for (fo++; fo != freeO.end() && prevFo->base + prevFo->use == fo->base; ) {
      if (dbg) logI("  - mergfreeO %zu + %zu with %zu + %zu\n", prevFo->base, prevFo->use, fo->base, fo->use);
      prevFo->use += fo->use;
      fo = freeO.erase_after(prevFo);
    }
    break;
  }
  a.state_ = INVALID;
  return 0;
}

int Library::write(command::SubmitInfo& info, VertexWriteFn writeFn,
                   void* userData) {
  if (vFence) {
    // vFlight or iFlight still in progress.
#ifdef INTERNAL_SUBMIT_AND_FENCE
    VkResult v = vFence->getStatus();
    if (v != VK_SUCCESS) {
      if (v == VK_NOT_READY) {
        return 0;
      }
      return explainVkResult("vFence->getStatus()", v);
    }
#endif /*INTERNAL_SUBMIT_AND_FENCE*/

    // vFence is done
    if (uglue.stage.pool.unborrowFence(vFence)) {
      logE("write: uglue.stage.pool.unborrowFence(vFence) failed\n");
      return 1;
    }
    vFence.reset();
    vFlight.reset();
    iFlight.reset();
    for (auto i = child.begin(); i != child.end(); i++) {
      if (!*i) {
        logE("vFlight or iFlight: found null asset in set\n");
        return 1;
      }
      auto& a = **i;
      if (a.state() == ADD_WAIT) {
        a.state_ = READY;
      }
    }
  }

  size_t firstV{0}, firstO{0};
  VertexIndex batch;
  VertexIndex out;
  for (auto i = child.begin(); i != child.end(); ) {
    if (!*i) {
      logE("write: found null asset in set\n");
      return 1;
    }
    auto fbSize = uglue.shaders.dev.framebufs.size();
    auto& a = **i;
    if (a.state() == ADDED) {
      out.vert.clear();
      out.order.clear();
      if (a.toVertices(out)) {
        logE("write: toVertices failed\n");
        return 1;
      }
      if (out.vert.size() < 1) {
        // No asset is allowed to only produce indices. Also assets only get
        // toVertices() called once. There is not "update in place" of an
        // asset.
        logE("write: toVertices did not generate vertices\n");
        return 1;
      }
      if (out.order.size() < 1) {
        logE("write: toVertices did not generate indices\n");
        return 1;
      }
      if (alloc(firstV, firstO, batch, out, a)) {
        logE("write: alloc failed\n");
        return 1;
      }
    } else if (a.state() == DELETED) {
      a.inst.cmd.indexCount = 0;  // First tell the GPU to stop drawing it.
      a.state_ = DEL_WAIT;
      a.frameNumber = uglue.frameNumber;
    } else if (a.state() == DEL_WAIT &&
               // If all in-flight GPU frames have stopped drawing
               uglue.frameNumber >= a.frameNumber + fbSize - 1) {
      if (free(a)) {  // Now it is safe to free the vertices
        logE("write: free failed\n");
        return 1;
      }
      i = child.erase(i);
      continue;  // Do not perform i++ below
    }
    i++;
  }
  if (batch.vert.empty()) {
    return 0;
  }

  if (uglue.stage.mmap(vertexBuf, vertexSize * firstV,
                       vertexSize * batch.vert.size(), vFlight)) {
    logE("write: stage.mmap(vertexBuf) failed\n");
    return 1;
  }
  char* dst = reinterpret_cast<char*>(vFlight->mmap());
  if (!dst) {
    logE("write: vFlight->mmap failed\n");
    return 1;
  }
  for (size_t i = 0; i < batch.vert.size(); i++, dst += vertexSize) {
    if (writeFn(userData, batch.vert.at(i), dst)) {
      logE("write: writeFn failed\n");
      return 1;
    }
  }

  if (firstO + batch.order.size() > order.size()) {
    order.resize(firstO + batch.order.size());
    if (order.size() > maxIndices) {
      logE("BUG: order.size = %zu maxIndices = %zu\n", order.size(),
           maxIndices);
      return 1;
    }
  }
  memcpy(&order.at(firstO), batch.order.data(),
         sizeof(indicesType) * batch.order.size());
  if (uglue.stage.mmap(indexBuf, sizeof(indicesType) * firstO,
                       sizeof(indicesType) * batch.order.size(), iFlight)) {
    logE("write: stage.mmap(indexBuf) failed\n");
    return 1;
  }
  auto* idst = reinterpret_cast<indicesType*>(iFlight->mmap());
  if (!idst) {
    logE("write: iFlight->mmap failed\n");
    return 1;
  }
  memcpy(idst, &order.at(firstO), sizeof(indicesType) * batch.order.size());

  vFence = uglue.stage.pool.borrowFence();
  if (!vFence) {
    logE("write: uglue.stage.pool.borrowFence failed\n");
    return 1;
  }
  if (uglue.stage.flushButNotSubmit(vFlight)) {
    logE("write: uglue.stage.flushflushButNotSubmit(vFlight) failed\n");
    (void)uglue.stage.pool.unborrowFence(vFence);
    vFence.reset();
    return 1;
  }
  if (uglue.stage.flushButNotSubmit(iFlight)) {
    logE("write: uglue.stage.flushflushButNotSubmit(iFlight) failed\n");
    (void)uglue.stage.pool.unborrowFence(vFence);
    vFence.reset();
    return 1;
  }
  if (vFlight->canSubmit() || iFlight->canSubmit()) {
    command::CommandPool::lock_guard_t lock(uglue.stage.pool.lockmutex);
    if (vFlight->canSubmit()) {
      if (vFlight->end() || vFlight->enqueue(lock, info)) {
        logE("write: vFlight end or enqueue failed\n");
        return 1;
      }
    }
    if (iFlight->canSubmit()) {
      if (iFlight->end() || iFlight->enqueue(lock, info)) {
        logE("write: iFlight end or enqueue failed\n");
        return 1;
      }
    }
#ifdef INTERNAL_SUBMIT_AND_FENCE
    if (uglue.stage.pool.submit(lock, uglue.stage.poolQindex, {info},
                                vFence->vk)) {
      logE("write: submit failed\n");
      return 1;
    }
#endif /*INTERNAL_SUBMIT_AND_FENCE*/

    // vFence, vFlight, and iFlight are in progress until the next write().
    return 0;
  }

  if (uglue.stage.pool.unborrowFence(vFence)) {
    logE("write: did not need it, uglue.stage.pool.unborrowFence failed\n");
    return 1;
  }
  vFence.reset();
  vFlight.reset();
  iFlight.reset();
  return 0;
}

int Library::add(std::shared_ptr<BaseAsset> asset) {
  if (!asset) {
    logE("Library:add(null) invalid asset\n");
    return 1;
  }
  auto r = child.insert(asset);
  if (!r.second) {
    logE("Library::add: cannot insert duplicate asset\n");
    return 1;
  }
  // set state to ADDED, then write() will update the vertex and index bufs.
  asset->state_ = ADDED;
  return 0;
}

int Library::del(std::shared_ptr<BaseAsset> asset) {
  if (!asset) {
    logE("Library:del(null) invalid asset\n");
    return 1;
  }
  auto r = child.find(asset);
  if (r == child.end()) {
    logE("Library:del(): asset not found\n");
    return 1;
  }
  if (!*r) {
    logE("Library::del(): found asset, but it was null\n");
    return 1;
  }
  if ((*r)->state() != READY) {
    logE("Library::del(): state=%d not READY(%d) - cannot delete\n",
         (int)(*r)->state(), (int)READY);
    return 1;
  }
  (*r)->state_ = DELETED;
  return 0;
}

int Library::addVertexAndInstInputs(
      science::PipeBuilder& pipe,
      std::vector<VkVertexInputAttributeDescription> attrs) {
  if (pipe.vertexInputs.size()) {
    logE("addVertexAndInstInputs: pipe already has %zu vertex inputs\n",
         pipe.vertexInputs.size());
    return 1;
  }

  // Find where to start patching by looking at calculated sizes.
  size_t i = attrs.size();
  for (;;) {
    i--;
    if (attrs.at(i).offset == vertexSize) {
      break;
    }
    if (i < 1 || attrs.at(i).offset < vertexSize) {
      logE("Failed to find offset %zu, got [%zu].offset = %zu\n",
           (size_t)vertexSize, i, (size_t)attrs.at(i).offset);
      return 1;
    }
  }

  // Because GLSL puts all the inputs in binding = 0, attrs must now
  // be patched up to set binding = bindingIndexOfInstanceBuf.
  for (; i < attrs.size(); i++) {
    attrs.at(i).binding = bindingIndexOfInstanceBuf;
    // Make the offset relative to the start of the binding.
    attrs.at(i).offset -= vertexSize;
  }

  // This reimplements science::PipeBuilder::addVertexInput<T> with some
  // new behavior because of the two bindings.
  pipe.vertexInputs.emplace_back();
  auto& bind0 = pipe.vertexInputs.back();
  bind0.binding = attrs.at(0).binding;
  bind0.stride = vertexSize;
  bind0.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  pipe.vertexInputs.emplace_back();
  auto& bind1 = pipe.vertexInputs.back();
  bind1.binding = bindingIndexOfInstanceBuf;
  bind1.stride = instSize;
  bind1.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

  pipe.attributeInputs.clear();
  pipe.attributeInputs.insert(pipe.attributeInputs.begin(),
                              attrs.begin(), attrs.end());

  // pipe is a PipeBuilder, the bindings and attributes are also
  // set in the underlying PipelineCreateInfo using C pointers:
  auto& verti = pipe.info().vertsci;
  verti.vertexBindingDescriptionCount = pipe.vertexInputs.size();
  verti.pVertexBindingDescriptions = pipe.vertexInputs.data();
  verti.vertexAttributeDescriptionCount = pipe.attributeInputs.size();
  verti.pVertexAttributeDescriptions = pipe.attributeInputs.data();
  return 0;
}

}  // namespace asset
