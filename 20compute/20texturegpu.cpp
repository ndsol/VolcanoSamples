/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * Implementation of TextureGPU class, which does the actual compression.
 */

#include "20texture.h"

// spv_20texturekernel_comp (SPIR-V bytecode) from glslangValidator:
#include "20compute/20texturekernel.comp.h"

namespace comp {
#include "20compute/struct_20texturekernel.comp.h"
}  // namespace comp

namespace example {

// localWorkSize is the product of the 3 dimensions of local_size_x, y, and z
// defined in 20textureetc2.comp. This is the number of local invocations that
// the GPU will *always* run. That's why in the compute shader, main() first
// checks if gl_GlobalInvocationID.x is greater than the size given in the UBO:
// any threads not needed must check and exit before they read or write from
// an invalid index.
//
// localWorkSize is used here to create ComputeBlock::i and o buffers sized so
// each thread gets a different chunk of the work.
constexpr uint32_t localWorkSize = comp::gl_WorkGroupSize_x *
                                   comp::gl_WorkGroupSize_y *
                                   comp::gl_WorkGroupSize_z;

constexpr uint32_t blockRows = 4;

// maxGroup is how many localWorkSize chunks are available in one block.
constexpr uint32_t maxGroup = 64;

// https://khronos.org/registry/DataFormat/specs/1.1/dataformat.1.1.html#ETC1
// Table 61 and Table 64 contain the same values.
static const uint32_t
    intensity_modifier[sizeof(comp::UBO::intensity_modifier) /
                       sizeof(comp::UBO::intensity_modifier[0])] = {
        (5u << 24) + (17u << 16) + (2u << 8) + 8u,
        (13u << 24) + (42u << 16) + (9u << 8) + 29u,
        (24u << 24) + (80u << 16) + (18u << 8) + 60u,
        (47u << 24) + (183u << 16) + (33u << 8) + 106u,
};

#define pack(a, b, c, d) ((a << 9) | (b << 6) | (c << 3) | d)
#define pack2(a, b, c, d) (pack(a, b, c, d) << 12)

// FIXME: Is this needed for UASTC?
//
// basisuFitOrder is used to find the base color (and from that the
// unscaled_color and the scaled_color). basisuFitOrder has 4 values used as
// weights multiplied by the best-so-far intensity_modifier added to the
// best-so-far base_color. That makes a new base_color which is compared to it.
//
// The weights sometimes get called "selectors," even though they range 0-8 and
// are multiplied by the error value - vs. a selector range 0-7 from the ETC1
// spec which selects one of intensity_modifier[] to add to base_color...
//
// Order matters here, since the algorithm is doing a search of the entire
// solution space. A good initial guess feeds into all the succeeding steps.
static const uint32_t basisuFitOrder[] = {
    pack(0, 0, 0, 8) + pack2(0, 5, 2, 1), pack(0, 6, 1, 1) + pack2(0, 7, 0, 1),
    // ETC1_Q_FAST stops after only testing the above weights
    pack(0, 7, 1, 0) + pack2(0, 0, 8, 0), pack(0, 0, 3, 5) + pack2(0, 1, 7, 0),
    pack(0, 0, 4, 4) + pack2(0, 0, 2, 6), pack(0, 0, 7, 1) + pack2(0, 0, 1, 7),
    pack(0, 0, 5, 3) + pack2(1, 6, 0, 1), pack(0, 0, 6, 2) + pack2(0, 2, 6, 0),
    pack(2, 4, 2, 0) + pack2(0, 3, 5, 0), pack(3, 3, 1, 1) + pack2(4, 2, 0, 2),
    pack(1, 5, 2, 0) + pack2(0, 5, 3, 0), pack(0, 6, 2, 0) + pack2(2, 4, 1, 1),
    pack(5, 1, 0, 2) + pack2(6, 1, 1, 0), pack(3, 3, 0, 2) + pack2(6, 0, 0, 2),
    pack(0, 8, 0, 0) + pack2(6, 1, 0, 1), pack(0, 1, 6, 1) + pack2(1, 6, 1, 0),
    // ETC1_Q_MED stops after only testing the above weights
    pack(4, 1, 3, 0) + pack2(0, 2, 5, 1), pack(5, 0, 3, 0) + pack2(5, 3, 0, 0),
    pack(0, 1, 5, 2) + pack2(0, 3, 4, 1), pack(2, 5, 1, 0) + pack2(1, 7, 0, 0),
    pack(0, 1, 4, 3) + pack2(6, 0, 2, 0), pack(0, 4, 4, 0) + pack2(2, 6, 0, 0),
    pack(0, 2, 4, 2) + pack2(0, 5, 1, 2), pack(0, 6, 0, 2) + pack2(3, 5, 0, 0),
    pack(0, 4, 3, 1) + pack2(3, 4, 1, 0), pack(4, 3, 1, 0) + pack2(1, 5, 0, 2),
    pack(0, 3, 3, 2) + pack2(1, 4, 1, 2), pack(0, 4, 2, 2) + pack2(2, 3, 3, 0),
    pack(4, 4, 0, 0) + pack2(1, 2, 4, 1), pack(0, 5, 0, 3) + pack2(0, 1, 3, 4),
    pack(1, 5, 1, 1) + pack2(1, 4, 2, 1), pack(1, 3, 2, 2) + pack2(5, 2, 1, 0),
    pack(1, 3, 3, 1) + pack2(0, 1, 2, 5), pack(1, 1, 5, 1) + pack2(0, 3, 2, 3),
    pack(2, 5, 0, 1) + pack2(3, 2, 2, 1), pack(2, 3, 0, 3) + pack2(1, 4, 3, 0),
    pack(2, 2, 1, 3) + pack2(6, 2, 0, 0), pack(1, 0, 6, 1) + pack2(3, 3, 2, 0),
    pack(7, 1, 0, 0) + pack2(3, 1, 4, 0), pack(0, 2, 3, 3) + pack2(0, 4, 1, 3),
    pack(0, 4, 0, 4) + pack2(0, 1, 0, 7), pack(2, 0, 5, 1) + pack2(2, 0, 4, 2),
    pack(3, 0, 2, 3) + pack2(2, 2, 4, 0), pack(2, 2, 3, 1) + pack2(4, 0, 3, 1),
    pack(3, 2, 3, 0) + pack2(2, 3, 2, 1), pack(1, 3, 4, 0) + pack2(7, 0, 1, 0),
    // ETC1_Q_SLOW stops after only testing the above weights
    pack(3, 0, 4, 1) + pack2(1, 0, 5, 2), pack(8, 0, 0, 0) + pack2(3, 0, 1, 4),
    pack(4, 1, 1, 2) + pack2(4, 0, 2, 2), pack(1, 2, 5, 0) + pack2(4, 2, 1, 1),
    pack(3, 4, 0, 1) + pack2(2, 0, 3, 3), pack(5, 0, 1, 2) + pack2(5, 0, 0, 3),
    pack(2, 4, 0, 2) + pack2(2, 1, 4, 1), pack(4, 0, 1, 3) + pack2(2, 1, 5, 0),
    pack(4, 2, 2, 0) + pack2(4, 0, 4, 0), pack(1, 0, 4, 3) + pack2(1, 4, 0, 3),
    pack(3, 0, 3, 2) + pack2(4, 3, 0, 1), pack(0, 1, 1, 6) + pack2(1, 3, 1, 3),
    pack(0, 2, 2, 4) + pack2(2, 0, 2, 4), pack(5, 1, 1, 1) + pack2(3, 0, 5, 0),
    pack(2, 3, 1, 2) + pack2(3, 0, 0, 5), pack(0, 3, 1, 4) + pack2(5, 0, 2, 1),
    pack(2, 1, 3, 2) + pack2(2, 0, 6, 0), pack(3, 1, 3, 1) + pack2(5, 1, 2, 0),
    pack(1, 0, 3, 4) + pack2(1, 1, 6, 0), pack(4, 0, 0, 4) + pack2(2, 0, 1, 5),
    pack(0, 3, 0, 5) + pack2(1, 3, 0, 4), pack(4, 1, 2, 1) + pack2(1, 2, 3, 2),
    pack(3, 1, 0, 4) + pack2(5, 2, 0, 1), pack(1, 2, 2, 3) + pack2(3, 2, 1, 2),
    pack(2, 2, 2, 2) + pack2(6, 0, 1, 1), pack(1, 2, 1, 4) + pack2(1, 1, 4, 2),
    pack(3, 2, 0, 3) + pack2(1, 2, 0, 5), pack(1, 0, 7, 0) + pack2(3, 1, 2, 2),
    pack(1, 0, 2, 5) + pack2(2, 0, 0, 6), pack(2, 1, 1, 4) + pack2(2, 2, 0, 4),
    pack(1, 1, 3, 3) + pack2(7, 0, 0, 1), pack(1, 0, 0, 7) + pack2(2, 1, 2, 3),
    pack(4, 1, 0, 3) + pack2(3, 1, 1, 3), pack(1, 1, 2, 4) + pack2(2, 1, 0, 5),
    pack(1, 0, 1, 6) + pack2(0, 2, 1, 5), pack(0, 2, 0, 6) + pack2(1, 1, 1, 5),
    pack(1, 1, 0, 6),
    // ETC1_Q_UBER tries all of the above. NOTE: This is not all possibilities!
};
#undef pack
#undef pack2

static std::vector<int> get_intensity_modifier(int i) {
  // ubo.intensity_modifier packs two rows into one
  uint raw = intensity_modifier[(i >> 1) & 3];
  raw >>= (i & 1) << 4;

  // Now raw contains two 8-bit values
  // e.g. if i == 0, raw = 0x208 which encodes [ 2, 8 ]
  int r0 = (raw >> 8) & 255, r1 = raw & 255;

  // Finally, derive the negative table columns from the positive columns.
  return std::vector<int>{-r1, -r0, r0, r1};
}

// TODO: when auto-tuning, first retire all the in-progress work, then
// delete and reallocate all the buffers with a bigger multiple of
// localWorkSize.
TextureGPU::TextureGPU(Compressor& parent)
    : parent{parent},
      compute{
          parent.cpool.vk.dev,
          sizeof(comp::UastcSource) * localWorkSize * maxGroup /*blockInSize*/,
          sizeof(comp::etc2block) * localWorkSize * maxGroup /*blockOutSize*/,
          comp::bindingIndexOfUBO(), sizeof(comp::UBO)},
      computeStage{compute.cpool, memory::ASSUME_POOL_QINDEX} {
  computeStage.sources.emplace_back(compute.cpool);
}

TextureGPU::~TextureGPU() {
  if (compute.doneBlocks.empty()) {
    return;
  }
  for (size_t i = 0; i < compute.doneBlocks.size(); i++) {
    auto& b = *compute.doneBlocks.at(i);
    if (b.flight) {
      b.flight.reset();
    }
  }
  if (compute.deleteBlocks(compute.doneBlocks)) {
    logE("~TextureGPU: compute.deleteBlocks failed\n");
  }
}

int Compressor::initCompute() {
  if (textureGPU.compute.shader->setName("compute.shader") ||
      textureGPU.compute.shader->loadSPV(spv_20texturekernel_comp,
                                         sizeof(spv_20texturekernel_comp))) {
    logE("textureGPU.compute.shader failed\n");
    return 1;
  }
  textureGPU.compute.uniform.emplace_back(cpool.vk.dev);
  if (textureGPU.compute.ctorError()) {
    logE("textureGPU.compute.ctorError failed\n");
    return 1;
  }
  textureGPU.step = TextureGPU::S_GRAY;
  return textureGPU.moreWorkSubmit();
}

int TextureGPU::moreWorkGray() {
  if (step != S_GRAY) {
    logE("moreWorkGray: must only be called in S_GRAY(%d), not %d\n", S_GRAY,
         step);
    return 1;
  }
  // moreWorkGray first fills prep, but sometimes does not empty it (such as
  // when waiting to writeUbo.) Only refill prep if it is empty. Also only
  // refill prep if doneRows indicates there can be more to add to prep.
  if (doneRows < inputH && prep.empty()) {
    prep = compute.newBlocks(1);
    if (prep.size() != 1) {
      logE("moreWorkGray: newBlocks failed: prep.size = %zu\n", prep.size());
      return 1;
    }
    auto& b = *prep.at(0);
    if (computeStage.mmap(b.i, 0, compute.blockInSize, b.flight)) {
      logE("computeStage.mmap(b.i) failed\n");
      return 1;
    }
    // moreWorkOneBlock sets b.work.x not just as a Work Group count, but the
    // number of local invocations (localWorkSize * maxGroup). ComputePipeline
    // needs b.work.x to be divided by localWorkSize and rounded up - below.
    b.work.y = 1;
    b.work.z = 1;
    b.work.x = 0;
    // moreWorkOneBlock may find the entire image is all one color. To do that,
    // call it until it either runs out of image pixels or adds to b.work.x.
    while (b.work.x == 0) {
      if (doneRows >= inputH) {
        // moreWorkOneBlock ran out of image pixels.
        if (compute.deleteBlocks(prep)) {
          logE("moreWorkGray: deleteBlocks(prep empty) failed\n");
          return 1;
        }
        prep.clear();
        break;
      }
      if (moreWorkOneBlock(b)) {
        logE("moreWorkOneBlock failed\n");
        return 1;
      }
    }
  }

  // Now that some work is in prep, decide whether to hand it to the GPU. If
  // waiting to writeUbo, the work stays in prep. Or, all the work may be done -
  // doneRows may indicate that - but it may not be time to advance the
  //'step' variable yet, such as if waiting for the GPU to finish the last
  // block.
  if (!prep.empty()) {
    auto& b = *prep.at(0);
    if (b.work.x < 1 || b.work.y != 1 || b.work.z != 1) {
      logE("moreWorkOneBlock chose invalid work = %zu %zu %zu\n",
           (size_t)b.work.x, (size_t)b.work.y, (size_t)b.work.z);
      return 1;
    }

    command::SubmitInfo info;
    // If uniform has not been initialized, initialize it.
    bool writeUbo = true;
    if (uboWorkCount) {
      if (b.work.x == uboWorkCount) {
        writeUbo = false;  // This is the normal case.
      } else {
        // Must writeUbo, but wait until all in-flight compute tasks are done.
        // This is inefficient because it forces the CPU to wait for the GPU,
        // but it is only done for the last block.
        if (!compute.runBlocks.empty()) {
          bool timeout;
          if (compute.wait(10u /*10ms*/ * 1000000lu, timeout)) {
            logE("compute.wait(10ms) failed\n");
            return 1;
          }
          return 0;
        }
        // Also wait until the block has been processed on the CPU.
        if (!compute.doneBlocks.empty()) {
          return 0;
        }
      }
    }
    if (writeUbo) {
      // Just blindly calling uboflight.reset is dangerous if it is in use on
      // the GPU. Wait above first for all in-flight compute tasks to be done.
      //
      // A better solution would be to keep a reference to the block that will
      // run uboflight, and when retiring blocks from doneBlocks, check if the
      // block is the one that ran uboflight. When uboflight is done, then do a
      // uboflight.reset(), and if the code gets to this point with uboflight
      // still holding a flight, it indicates a race condition.
      uboflight.reset();
      if (computeStage.mmap(compute.uniform.at(0), 0, compute.uboSize,
                            uboflight)) {
        logE("computeStage.mmap(uboflight) failed\n");
        return 1;
      }
      auto* ubo = reinterpret_cast<comp::UBO*>(uboflight->mmap());
      if (!ubo) {
        logE("uboflight->mmap failed\n");
        return 1;
      }

      memset(ubo, 0, sizeof(*ubo));
      for (size_t i = 0; i < sizeof(ubo->intensity_modifier) /
                                 sizeof(ubo->intensity_modifier[0]);
           i++) {
        ubo->intensity_modifier[i] = intensity_modifier[i];
      }
      ubo->workCount = b.work.x;  // Validated above that b.work.yz are 1.
      uboWorkCount = ubo->workCount;
      if (computeStage.flushButNotSubmit(uboflight)) {
        logE("computeStage.flushButNotSubmit(uboflight) failed\n");
        return 1;
      }
      science::ComputePipeline::lock_guard_t lock(compute.lockmutex);
      if (uboflight->canSubmit()) {
        if (uboflight->end() || uboflight->enqueue(lock, info)) {
          logE("uboflight->end or enqueue failed\n");
          return 1;
        }
      }
    }
    logI("moreWorkGray: S_GRAY doneRows=%zu inputH=%zu\n", doneRows, inputH);
    // Correct b.work.x to what ComputePipeline expects
    b.work.x = (b.work.x + localWorkSize - 1) / localWorkSize;

    // Write the input blocks for the shader threads.
    if (computeStage.flushButNotSubmit(b.flight)) {
      logE("computeStage.flushButNotSubmit(b.flight) failed\n");
      return 1;
    }
    if (compute.enqueueBlocks(prep, info)) {
      logE("moreWorkGray: enqueueBlocks failed\n");
      return 1;
    }
    prep.clear();
  }
  return 0;
}

#define UASTC_SRC_FLAG_ALL_SAME_COLOR (1)
#define UASTC_SRC_FLAG_HAS_ALPHA (2)
#define UASTC_SRC_FLAG_HAS_COLOR (4)
static inline uint32_t update_flags(uint32_t flags, uint32_t pixel) {
  uint32_t alpha = pixel & 0xff000000u;
  flags |= (alpha != 0xff000000u) * UASTC_SRC_FLAG_HAS_ALPHA;
  // Swap the bits in alpha - it had only the high 8 bits...
  alpha ^= pixel;
  // ... now alpha has the low 24 bits (the R, G, B values).
  flags |= (alpha != (alpha & 0xff) * 0x10101u) * UASTC_SRC_FLAG_HAS_COLOR;
  return flags;
}

int TextureGPU::moreWorkOneBlock(science::ComputeBlock& b) {
  // if cachedRows does not have enough for a block's height (blockRows)
  size_t inCache = cachedRows.size() / inputW;
  if (inCache - doneRowsInCache < blockRows) {
    if (!cachedRows.empty()) {
      // move any remaining rows in cachedRows up to row 0
      memmove(&cachedRows[0], &cachedRows.at(inputW * doneRowsInCache),
              cachedRows.size() - inputW * doneRowsInCache);
      inCache -= doneRowsInCache;
      doneRowsInCache = 0;
    }

    // fill cachedRows with more rows.
    // This moves data through several stages each with a different size limit:
    // 1. scanSize is how many lines to ask Skia to read from the image.
    //    Later, scanSize can be even smaller for the last few "odd" rows.
    // 2. wantRows is how many lines can be added to cachedRows.
    //    Later, cachedRows may be downsized if fewer rows were read (inCache).
    // 3. Finally the number of blocks is limited, so doneRowsInCache is only
    //    updated once all the pixels in the rows are stuffed into blocks.
    size_t wantRows = computeStage.mmapMax() / decoder.stride;
    if (wantRows < blockRows) {
      wantRows = blockRows;
    }
    cachedRows.resize(wantRows * decoder.stride);

    for (size_t scanSize = wantRows - inCache; decoder.lineCount < inputH;) {
      if (decoder.lineCount + scanSize > inputH) {
        scanSize = inputH - decoder.lineCount;  // read last few "odd" rows.
      }
      if (decoder.read(scanSize, &cachedRows.at(inCache * inputW))) {
        logE("moreWorkOneBlock: decoder.read(%zu) failed\n", scanSize);
        return 1;
      }
      inCache += scanSize;
    }
    if (inCache < wantRows) {
      // Downsize cachedRows because it is not full.
      cachedRows.resize(inCache * inputW);
    }
  }

  auto* blockIn = reinterpret_cast<comp::UastcSource*>(b.flight->mmap());
  if (!blockIn) {
    logE("b.flight->mmap failed\n");
    return 1;
  }

  // Stuff pixels into blocks. This is a "hot spot" because it is copying
  // individual pixels. Special cases are handled separately for if inputW or
  // inputH are not an even multiple of blockRows.
  size_t copiedRows = doneRowsInCache;
  const auto* cache = &cachedRows.at(copiedRows * inputW);
  if (copiedRows + blockRows < inCache) {
    // cachedRows has enough rows for a block.
    for (; b.work.x < localWorkSize * maxGroup;) {
      uint32_t flags = 0;
      auto* local = &blockIn[b.work.x].pixel[0];
      auto* ycache = &cache[copiedCols];
      for (size_t y = 0; y < blockRows; y++, ycache += inputW) {
        for (size_t x = 0; x < blockRows; x++) {
          local[y * blockRows + x] = ycache[x];
          flags = update_flags(flags, ycache[x]);
        }
      }
      if (addBlockCoord(b, blockIn[0].pixel, local, flags, copiedCols,
                        doneRows + copiedRows)) {
        logE("moreWorkOneBlock: addBlockCoord failed\n");
        return 1;
      }
      b.work.x++;
      copiedCols += blockRows;
      if (copiedCols + blockRows > inputW) {
        if (copiedCols < inputW) {
          // Image pixels on the right do not end on a clean block boundary.
          flags = 0;
          local = &blockIn[b.work.x].pixel[0];
          size_t canCopyX = inputW - copiedCols;
          ycache = &cache[copiedCols];
          for (size_t y = 0; y < blockRows; y++, ycache += inputW) {
            size_t x = 0;
            for (; x < canCopyX; x++) {
              local[y * blockRows + x] = ycache[x];
              flags = update_flags(flags, ycache[x]);
            }
            for (; x < blockRows; x++) {  // Repeat the last column.
              local[y * blockRows + x] = ycache[canCopyX - 1];
            }
          }
          if (addBlockCoord(b, blockIn[0].pixel, local, flags, copiedCols,
                            doneRows + copiedRows)) {
            logE("moreWorkOneBlock: addBlockCoord failed\n");
            return 1;
          }
        }
        copiedCols = 0;
        copiedRows += blockRows;
        cache += blockRows * inputW;
        if (copiedRows + blockRows > inCache) {
          break;
        }
      }
    }
  }
  if (copiedRows < inCache) {
    // Special case: cachedRows does not have enough rows for a block.
    while (b.work.x < localWorkSize * maxGroup) {
      uint32_t flags = 0;
      auto* local = &blockIn[b.work.x].pixel[0];
      size_t canCopyX = inputW - copiedCols;
      auto* ycache = &cache[copiedCols];
      size_t y = 0;
      size_t rows = copiedRows;
      for (; rows < inCache; y++, rows++, cache += inputW, ycache += inputW) {
        size_t x = 0;
        for (; x < canCopyX; x++) {
          local[y * blockRows + x] = ycache[x];
          flags = update_flags(flags, ycache[x]);
        }
        for (; x < blockRows; x++) {  // Repeat the last column.
          local[y * blockRows + x] = ycache[canCopyX - 1];
        }
      }
      for (; y < blockRows; y++) {  // Repeat the last row
        size_t x = 0;
        for (; x < canCopyX; x++) {
          local[y * blockRows + x] = ycache[x];
        }
        for (; x < blockRows; x++) {  // Repeat the last column.
          local[y * blockRows + x] = ycache[canCopyX - 1];
        }
      }
      if (addBlockCoord(b, blockIn[0].pixel, local, flags, copiedCols,
                        doneRows + copiedRows)) {
        logE("moreWorkOneBlock: addBlockCoord failed\n");
        return 1;
      }
      // Only updated copiedRows after addBlockCoord uses it for the block coord
      copiedRows = rows;
    }
  }
  doneRows += copiedRows - doneRowsInCache;
  doneRowsInCache = copiedRows;

  // If b.work.x reached localWorkSize * maxGroup with copiedCols still pointing
  // to part of a row, this function will resume copying pixels at that point
  // the next time it is called.
  return 0;
}

static bool usedInThisStep(TextureGPU::TextureSteps step, uint32_t flags) {
  if (step == TextureGPU::S_GRAY) {
    return !(flags & UASTC_SRC_FLAG_HAS_COLOR);
  }
  if (step == TextureGPU::S_SOLID) {
    return !(flags & UASTC_SRC_FLAG_HAS_ALPHA);
  }
  if (step == TextureGPU::S_ALPHA) {
    return !!(flags & UASTC_SRC_FLAG_HAS_ALPHA);
  }
  logF("usedInThisStep: step %d not supported\n", step);
  return true;
}

// addBlockCoord remembers which parts of the image are in this block.
int TextureGPU::addBlockCoord(science::ComputeBlock& b, unsigned* blockIn,
                              unsigned* local, uint32_t flags, size_t inputX,
                              size_t inputY) {
  // Check if all pixels in the block are the same color.
  unsigned first_color = local[0];
  for (size_t i = 1; i < blockRows * blockRows; i++) {
    if (local[i] != first_color) {
      // Check if this block is included in this step.
      if (!usedInThisStep(step, flags)) {
        delayedWork.emplace_back(inputX, inputY, flags);
        memcpy(&delayedWork.back().pixel[0], &local[0],
               sizeof(delayedWork[0].pixel));
        return 0;
      }
      b.work.x++;
      return 0;
    }
  }
  // All pixels are the same color. That check could have been done on the GPU,
  // of course, but that would just make that local invocation disable itself,
  // wasting a GPU core. Also, the data is available in the CPU cache right now,
  // making it worth the simple check to eliminate a whole block now.
  flags |= UASTC_SRC_FLAG_ALL_SAME_COLOR;
  delayedWork.emplace_back(inputX, inputY, flags);
  memcpy(&delayedWork.back().pixel[0], &local[0], sizeof(delayedWork[0].pixel));
  return 0;
}

int TextureGPU::moreWorkSubmit() {
  if (step == INVALID) {
    logE("moreWorkSubmit: must set step to something first\n");
    return 1;
  }

  auto nextStep = step;
  if (step == S_GRAY) {
    if (moreWorkGray()) {
      return 1;
    }
    if (prep.empty() && doneRows >= inputH) {
      nextStep = S_SOLID;
    }
  } else if (step == S_SOLID) {
    // moreWorkGray in S_GRAY fills delayedWork. Pull from that now.
    logI("S_SOLID: %zu delayedWork\n", delayedWork.size());
    nextStep = S_ALPHA;
  } else if (step == S_ALPHA) {
    logI("S_ALPHA: %zu delayedWork\n", delayedWork.size());
    nextStep = S_PACK;
  } else if (step == S_PACK) {
    // All done!
    nextStep = DONE;
  } else {
    logE("moreWorkSubmit: step = %d\n", step);
    return 1;
  }
  if (nextStep != step) {
    // Wait until blocks are done on the GPU.
    if (compute.runBlocks.empty() && compute.doneBlocks.empty()) {
      logI("transition from %d to %d\n", step, nextStep);
      step = nextStep;
    }
  }
  return 0;
}

int TextureGPU::poll() {
  if (compute.poll()) {
    logE("poll: compute.poll failed\n");
    return 1;
  }
  while (!compute.doneBlocks.empty()) {
    // Attempt to schedule more GPU work right away, if doneRows is not maxed.
    if (moreWorkSubmit()) {
      logE("poll: early moreWorkSubmit failed\n");
      return 1;
    }

    auto& b = *compute.doneBlocks.at(0);
    if (b.flight) {
      b.flight.reset();
    }
    // read from b.i
    std::shared_ptr<memory::Flight> readf;
    if (computeStage.read(b.o, 0, compute.blockOutSize, readf)) {
      logE("computeStage.read(readf) failed\n");
      return 1;
    }
    if (!readf) {
      logE("computeStage.read returned null\n");
      return 1;
    }
    if (computeStage.flushButNotSubmit(readf)) {
      logE("computeStage.flushButNotSubmit(readf) failed\n");
      return 1;
    }
    if (readf->canSubmit()) {
      std::shared_ptr<command::Fence> fence{compute.cpool.borrowFence()};
      science::ComputePipeline::lock_guard_t lock(compute.lockmutex);
      command::SubmitInfo info;
      if (readf->end() || readf->enqueue(lock, info)) {
        logE("readf.end or enqueue failed\n");
        return 1;
      }
      if (compute.cpool.submit(lock, compute.poolQindex, {info}, fence->vk)) {
        logE("ComputePipeline::cpool.submit failed\n");
        (void)compute.cpool.unborrowFence(fence);
        return 1;
      }
      logI("poll: read submitted, wait...\n");
      VkResult v = fence->waitMs(100);
      if (v != VK_SUCCESS) {
        return explainVkResult("fence->waitMs(100)", v);
      }
      if (compute.cpool.unborrowFence(fence)) {
        logE("poll: compute.cpool.unborrowFence(fence) failed\n");
        return 1;
      }
    }
    comp::etc2block* blockOut = (comp::etc2block*)readf->mmap();
    if (!blockOut) {
      logE("readf->mmap failed\n");
      return 1;
    }
    size_t errs = 0;
    for (size_t i = 0; i < b.work.x * localWorkSize; i++) {
      auto* localBlock = &blockOut[i];
      for (size_t j = 0;
           j < sizeof(localBlock->luma) / sizeof(localBlock->luma[0]); j++) {
        int want = i * 128 + (i & 127) + 1;
        if (j < 4) {
          want = get_intensity_modifier(i & 127).at(j);
        }
        if (localBlock->luma[j] != want) {
          logI("block[%zu][%zu]=%d want %d\n", i, j, (int)localBlock->luma[j],
               want);
          errs++;
          if (errs > 10) break;
        }
      }
      if (errs > 10) break;
    }

    // Remove the first block in compute.doneBlocks
    science::ComputePipeline::BlockVec v{compute.doneBlocks.at(0)};
    if (compute.deleteBlocks(v)) {
      logE("poll: compute.deleteBlocks failed\n");
      return 1;
    }
    v.clear();
  }

  if (step == S_GRAY) {
    // compute.doneBlocks is now empty. If GPU does not have work scheduled...
    if (compute.runBlocks.empty()) {
      // If work is waiting in prep or if doneRows is not all done yet...
      if (moreWorkSubmit()) {
        logE("poll: S_GRAY: moreWorkSubmit failed\n");
        return 1;
      }
    }
  } else if (step == S_SOLID) {
    if (moreWorkSubmit()) {
      logE("poll: S_SOLID: moreWorkSubmit failed\n");
      return 1;
    }
  } else if (step == S_ALPHA) {
    if (moreWorkSubmit()) {
      logE("poll: S_ALPHA: moreWorkSubmit failed\n");
      return 1;
    }
  } else if (step == S_PACK) {
    logI("poll: S_PACK run=%zu done=%zu\n", compute.runBlocks.size(),
         compute.doneBlocks.size());
    if (moreWorkSubmit()) {
      logE("poll: S_PACK: moreWork failed\n");
      return 1;
    }
  }
  return 0;
}

int TextureGPU::workDone() { return step == DONE; }

}  // namespace example
