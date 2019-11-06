/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * ScanlineDecoder loads an image using skia, using less memory than the simple
 * skiaglue method. It also allows displaying a progress meter.
 */

#pragma once

#include <src/science/science.h>

#include "SkCodec.h"
#include "SkData.h"
#include "SkImageInfo.h"
#include "SkRefCnt.h"

// ScanlineDecoderCPU is used by ScanlineDecoder
struct ScanlineDecoderCPU {
  ~ScanlineDecoderCPU() { (void)close(); }

  // open begins reading the given filename.
  // returns 0 if codec is valid, 1 on error.
  WARN_UNUSED_RESULT int open(const char* filename, size_t mmapMax);

  // close can be called to release any memory not already released. However,
  // any call to open calls close to reset the class state before starting the
  // new image. Also, when read() finishes, it calls close. It is likely
  // your app does not need to call close explicitly at all.
  WARN_UNUSED_RESULT int close() {
    codec.reset();
    increment.fRight = 0;
    return 0;
  }

  // read must not be called until open() succeeds.
  //
  // read() transfers image data to *sampler.image and updates lineCount.
  // The amount of lines read depends on the image format and mmapMax.
  int read(VkDeviceSize lines, void* out);

  // codec is the Skia class that implements the image decoder.
  std::unique_ptr<SkCodec> codec;

  // imgFilenameFound reports the actual path opened.
  std::string imgFilenameFound;

  // lineCount is the number of successfully read lines by read().
  VkDeviceSize lineCount{0};

  // stride is the number of bytes in a single line of the decoded image.
  VkDeviceSize stride{0};

  bool moreLines() const {
    if (!codec) {
      return lineCount == 0;
    }
    return lineCount < (size_t)codec->getInfo().height();
  }

 protected:
  int mode{0};
  SkIRect increment;
  void* prevOut{nullptr};
  static int handleSkiaError(SkCodec::Result r, const char* filename);
};

// ScanlineDecoder is useful for very large images. One day maybe JPEGs will
// be decoded on the GPU, but for now, this transfers the data to the GPU
// after it is decoded on the CPU.
//
// ScanlineDecoder saves memory because the image is not stored multiple times
// like how skiaglue does. Where skiaglue loads the entire compressed image,
// then decodes the entire image into host-coherent memory, then copies it
// to the GPU, ScanlineDecoder loads only a small number of scan lines, then
// copies them to the GPU.
//
// 'lineCount' and 'maxLines' also give your app something like a progress bar.
struct ScanlineDecoder {
  ScanlineDecoder(memory::Stage& stage) : stage(stage) {}

  ScanlineDecoderCPU cpu;

  // stage holds blocks of data being transferred to the GPU.
  memory::Stage& stage;

  // sampler.image is populated with the image as it is decoded.
  // This provides both a Sampler and an Image for whatever your app needs.
  science::Sampler sampler{stage.pool.vk.dev};

  // dstMipLevel can be customized after open() to transfer decoded texels to
  // a different mip level if desired.
  uint32_t dstMipLevel{0};

  // dstArrayLayer can be customized after open() to transfer decoded texels to
  // a different array layer if desired.
  uint32_t dstArrayLayer{0};

  // open begins reading the given filename.
  // When open returns, sampler.image->info.extent is valid.
  WARN_UNUSED_RESULT int open(const char* filename);

  // close can be called to release any memory not already released. However,
  // any call to open calls close to reset the class state before starting the
  // new image. Also, when read() finishes, it calls close. It is likely
  // your app does not need to call close explicitly at all.
  WARN_UNUSED_RESULT int close();

  // read must not be called until open() succeeds.
  //
  // read() transfers image data to *sampler.image and updates cpu.lineCount.
  // The amount of lines read depends on the image format and stage.mmapMax().
  //
  // The first call to read() will call sampler.image->ctorError().
  int read();

  bool moreLines() const { return cpu.moreLines(); }

 protected:
  // FlightTracker is used internally to track Flight objects.
  struct FlightTracker {
    FlightTracker(std::shared_ptr<command::Fence> fence,
                  std::shared_ptr<memory::Flight> flight)
        : fence(fence), flight(flight) {}
    std::shared_ptr<command::Fence> fence;
    std::shared_ptr<memory::Flight> flight;
  };

  // inFlight is used internally to track Flight objects.
  std::vector<FlightTracker> inFlight;

  int flushFlights();
};
