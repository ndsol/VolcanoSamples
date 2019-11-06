/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This sample demonstrates GPU-accelerated texture compression.
 */

#include <src/science/science.h>

#include "../src/scanlinedecoder.h"
#include "../src/uniformglue/uniformglue.h"

#pragma once

namespace example {

class Compressor;

typedef struct BlockAllSameColor {
  BlockAllSameColor(size_t x, size_t y, unsigned color)
      : x(x), y(y), color(color){};

  const size_t x;
  const size_t y;
  const unsigned color;
} BlockAllSameColor;

// The TextureGPU class does the actual compression. It is a member of
// the Compressor class which handles rendering a UI etc.
typedef struct TextureGPU {
  TextureGPU(Compressor& parent);
  ~TextureGPU();
  Compressor& parent;

  // inputW, inputH, doneRows, ... to cachedRows are managed by moreWorkSubmit()
  // and child function moreWorkOneBlock().
  size_t inputW{0}, inputH{0};
  size_t doneRows{0};
  size_t doneRowsInCache{0};
  size_t copiedCols{0};
  uint32_t uboWorkCount{0};
  std::vector<uint32_t> cachedRows;
  std::vector<BlockAllSameColor> blockAllSameColor;
  // prep holds blocks containing input data, not yet submitted to the GPU,
  // until they are submitted.
  science::ComputePipeline::BlockVec prep;

  int initCompute();
  int moreWorkSubmit();
  int moreWorkPrepComp();
  int moreWorkOneBlock(science::ComputeBlock& b);
  int addBlockCoord(science::ComputeBlock& b, unsigned* blockIn,
                    unsigned* local, size_t inputX, size_t inputY);
  int poll();
  int workDone();

  science::ComputePipeline compute;
  memory::Stage computeStage;
  std::shared_ptr<memory::Flight> uboflight;
  ScanlineDecoderCPU decoder;

  enum TextureSteps {
    INVALID = 0,
    S_COMP,
    S_PACK,
    DONE,
  };
  TextureSteps step{INVALID};
} TextureGPU;

// Options captures all command line options used in a Compressor run.
typedef struct Options {
  // filename is the input image file to read.
  std::string filename;
  // flagTui: if true, only use the text console.
  bool flagTui{false};
  // quality: anything from 0-9. However, right now only the following have
  // any meaning:
  // 0 = fastest, worst quality compression
  // 2 = medium speed
  // 4 = slow speed
  // 6 = slowest speed, best quality compression
  // (anything over 6 uses 6)
  int quality{0};
} Options;

// Compressor is the main app class. It reads an image file (see Options above)
// compresses it and writes it out.
class Compressor : public BaseApplication {
 public:
  Compressor(language::Instance& instance, GLFWwindow* window);
  int run(const Options& options);

 protected:
  // uglue and these other objects render the UI.
  UniformGlue uglue;
  science::PipeBuilder pipe0{pass};
  science::Sampler sampler{cpool.vk.dev};

  // TextureGPU is defined in "20texturegpu.h"
  TextureGPU textureGPU;

  int handleSkiaError(SkCodec::Result r, const char* filename);
  int buildPass();
  int buildFramebuf(language::Framebuf& framebuf, size_t framebuf_i);
  int redraw(std::shared_ptr<memory::Flight>& flight);
  int initUI();
  int initCompute();
};

}  // namespace example
