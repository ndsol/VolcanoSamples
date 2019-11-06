/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 *
 * Implementation of ScanlineDecoder.
 */

#include "scanlinedecoder.h"

#include <src/science/science.h>

int ScanlineDecoderCPU::handleSkiaError(SkCodec::Result r,
                                        const char* filename) {
  const char* msg;
  switch (r) {
    case SkCodec::kSuccess:
      return 0;
    case SkCodec::kIncompleteInput:
      logW("skia codec: %s \"%s\"\n", "incomplete image", filename);
      return 0;
    case SkCodec::kErrorInInput:
      logW("skia codec: %s \"%s\"\n", "errors in image", filename);
      return 0;
    case SkCodec::kInvalidConversion:
      msg = "unable to output in this pixel format";
      break;
    case SkCodec::kInvalidScale:
      msg = "unable to rescale the image to this size";
      break;
    case SkCodec::kInvalidParameters:
      msg = "invalid parameters or memory to write to";
      break;
    case SkCodec::kInvalidInput:
      msg = "invalid input";
      break;
    case SkCodec::kCouldNotRewind:
      msg = "could not rewind";
      break;
    case SkCodec::kInternalError:
      msg = "internal error (out of memory?)";
      break;
    case SkCodec::kUnimplemented:
      msg = "TODO: this method is not implemented";
      break;
    default:
      logE("skia codec: Result(%d) ??? \"%s\"\n", int(r), filename);
      return 1;
  }
  logE("skia codec: %s \"%s\"\n", msg, filename);
  return 1;
}

#ifdef __ANDROID__
#include <android_native_app_glue.h>

struct AndroidStream : public SkStreamAsset {
  AndroidStream() {}
  virtual ~AndroidStream() {
    if (aa) {
      AAsset_close(aa);
      aa = nullptr;
    }
  }
  AAsset* aa{nullptr};
  off64_t assetLen{0};

  // make calls AAssetManager_open(). The destructor calls AAsset_close().
  static std::unique_ptr<AndroidStream> make(const std::string& filename) {
    AAsset* aa = AAssetManager_open(glfwGetAndroidApp()->activity->assetManager,
                                    filename.c_str(), AASSET_MODE_STREAMING);
    if (!aa) {
      logE("ScanlineDecoder::open: unable to open \"%s\"\n", filename.c_str());
      return std::unique_ptr<AndroidStream>();
    }
    auto r = std::unique_ptr<AndroidStream>(new AndroidStream());
    r->aa = aa;
    r->assetLen = AAsset_getLength64(aa);
    if (r->assetLen < 0) {
      logE("ScanlineDecoder::open: unable to size \"%s\"\n", filename.c_str());
      return std::unique_ptr<AndroidStream>();
    }
    return r;
  }

  virtual size_t read(void* buffer, size_t size) {
    if (buffer) {
      return AAsset_read(aa, buffer, size);
    }
    // If buffer is NULL, just seek.
    if (AAsset_seek64(aa, size, SEEK_CUR)) {
      return 0;
    }
    return size;
  }

  virtual bool rewind() {
    if (AAsset_seek64(aa, 0, SEEK_SET)) {
      return false;
    }
    return true;
  }

  virtual bool hasPosition() const { return true; }
  virtual size_t getPosition() const {
    return (size_t)(assetLen - AAsset_getRemainingLength64(aa));
  }
  virtual bool seek(size_t position) {
    if (AAsset_seek64(aa, position, SEEK_SET)) {
      return false;
    }
    return true;
  }
  virtual bool move(long offset) {
    if (AAsset_seek64(aa, offset, SEEK_CUR)) {
      return false;
    }
    return true;
  }
  virtual bool isAtEnd() const { return AAsset_getRemainingLength64(aa) == 0; }
  virtual bool hasLength() { return true; }
  virtual size_t getLength() const { return (size_t)assetLen; }

 private:
  virtual SkStreamAsset* onDuplicate() const { return nullptr; }
  virtual SkStreamAsset* onFork() const { return nullptr; }
};
#endif

int ScanlineDecoderCPU::open(const char* filename, size_t mmapMax) {
  if (close()) {
    logE("open(%s): pre-close failed\n", filename);
    return 1;
  }
  lineCount = 0;

  // findInPaths is in volcano core/structs.h.
  if (findInPaths(filename, imgFilenameFound)) {
    logE("ScanlineDecoder::open(%s): file not found\n", filename);
    return 1;
  }

#ifdef __ANDROID__
  std::unique_ptr<AndroidStream> data(AndroidStream::make(imgFilenameFound));
#else
  std::unique_ptr<SkStreamAsset> data(
      SkStream::MakeFromFile(imgFilenameFound.c_str()));
#endif
  if (!data) {
    logE("ScanlineDecoder::open: unable to read \"%s\"\n",
         imgFilenameFound.c_str());
    return 1;
  }
  if (data->hasLength() && data->getLength() < 4) {
    // Catch this now. Skia reports kUnimplemented which is very confusing.
    logE("ScanlineDecoder::open: invalid file \"%s\"\n",
         imgFilenameFound.c_str());
    return 1;
  }

  SkCodec::Result r = SkCodec::kSuccess;
  codec = SkCodec::MakeFromStream(std::move(data), &r);
  if (handleSkiaError(r, imgFilenameFound.c_str()) || !codec) {
    logE("ScanlineDecoder::open: unable to create a codec \"%s\"\n",
         imgFilenameFound.c_str());
    codec.reset();
    return 1;
  }
  auto& info = codec->getInfo();
  // Skia only natively supports a few formats, all just swizzles of RGBA8 or
  // gray scale (which upconverts fine to RGBA8). Keep things simple.
  SkColorType colorType = kRGBA_8888_SkColorType;
  stride = 4 /*size of R8G8B8A8 format*/ * info.width();
  if (stride > mmapMax) {
    logE("ScanlineDecoder::open: image \"%s\" too large for stage %zu\n",
         imgFilenameFound.c_str(), mmapMax);
    codec.reset();
    return 1;
  }

  if (codec->getEncodedFormat() == SkEncodedImageFormat::kPNG) {
    mode = 1;  // kPNG requires incremental decode, not scanline decode.
    r = SkCodec::kSuccess;
    return 0;
  }

  mode = 0;
  auto skInfo = SkImageInfo::Make(info.width(), info.height(), colorType,
                                  info.alphaType());
  r = codec->startScanlineDecode(skInfo);
  if (handleSkiaError(r, imgFilenameFound.c_str())) {
    logE("startScanlineDecode failed\n");
    codec.reset();
    return 1;
  }
  return 0;
}

int ScanlineDecoderCPU::read(VkDeviceSize lines, void* out) {
  if (!codec) {
    logE("ScanlineDecoder::read: open must be called first\n");
    return 1;
  }
  if (!out) {
    logE("ScanlineDecoder::read: mmap failed\n");
    return 1;
  }
  if (!lines) {
    // Nothing left to read.
    return 0;
  }

  auto& ci = codec->getInfo();
  if (lineCount + lines > (VkDeviceSize)ci.height()) {
    logE("ScanlineDecoder::read: at end of file\n");
    return 1;
  }

  int r;
  if (mode == 0) {
    r = codec->getScanlines(out, lines, stride);
  } else if (increment.fRight == 0) {
    increment.fLeft = 0;
    increment.fTop = lineCount;
    increment.fRight = ci.width();
    increment.fBottom = lineCount + lines;

    SkCodec::Options opt;
    opt.fSubset = &increment;
    SkColorType colorType = kRGBA_8888_SkColorType;
    auto skInfo =
        SkImageInfo::Make(ci.width(), ci.height(), colorType, ci.alphaType());

    SkCodec::Result s =
        codec->startIncrementalDecode(skInfo, out, stride, &opt);
    if (handleSkiaError(s, imgFilenameFound.c_str())) {
      logE("ScanlineDecoder::read: unable to start codec \"%s\"\n",
           imgFilenameFound.c_str());
      codec.reset();
      return 1;
    }

    r = 0;
    s = codec->incrementalDecode(&r);
    if (s == SkCodec::kSuccess) {
      r = lines;
    } else if (s == SkCodec::kIncompleteInput) {
      // This indicates r is valid, and the file could not be read.
      if (r < 0 || (VkDeviceSize)r >= lines) {
        r = 0;
      }
    } else {
      r = 0;
      if (handleSkiaError(s, imgFilenameFound.c_str())) {
        logE("ScanlineDecoder::read: incremental failed: \"%s\"\n",
             imgFilenameFound.c_str());
        codec.reset();
        return 1;
      }
    }
  }
  if (r != (int)lines) {
    logE("ScanlineDecoder::read: getScanlines got %d, want %zu\n", r,
         (size_t)lines);
    return 1;
  }
  lineCount += lines;
  return 0;
}

int ScanlineDecoder::open(const char* filename) {
  if (close()) {
    logE("open(%s): pre-close failed\n", filename);
    return 1;
  }
  if (cpu.open(filename, stage.mmapMax())) {
    return 1;
  }
  if (!cpu.codec) {
    logE("ScanlineDecoder::open: cpu.open left codec null\n");
    return 1;
  }
  auto& info = cpu.codec->getInfo();
  auto& si = sampler.image->info;
  si.extent.width = info.width();
  si.extent.height = info.height();
  si.extent.depth = 1;

  VkFormatFeatureFlags flags =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
      VK_FORMAT_FEATURE_BLIT_DST_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if (sampler.vk.dev.apiVersionInUse() >= VK_MAKE_VERSION(1, 1, 0)) {
    flags |=
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  }
  si.format = sampler.vk.dev.chooseFormat(
      si.tiling, flags, VK_IMAGE_TYPE_2D,
      {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM});
  if (si.format == VK_FORMAT_UNDEFINED) {
    logE("ScanlineDecoder::open: no format supports BLIT_DST\n");
    return 1;
  }
  if (sampler.image->setMipLevelsFromExtent()) {
    logE("ScanlineDecoder::open: setMipLevelsFromExtent failed\n");
    return 1;
  }
  si.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
             VK_IMAGE_USAGE_SAMPLED_BIT;
  sampler.info.maxLod = si.mipLevels;
  sampler.imageView.info.subresourceRange =
      sampler.image->getSubresourceRange();

  sampler.info.magFilter = VK_FILTER_LINEAR;
  sampler.info.minFilter = VK_FILTER_LINEAR;
  sampler.info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  dstMipLevel = 0;
  dstArrayLayer = 0;
  return 0;
}

int ScanlineDecoder::read() {
  if (!cpu.codec) {
    logE("ScanlineDecoder::read: open must be called first\n");
    return 1;
  }
  if (flushFlights()) {
    logE("ScanlineDecoder::read: flushFlights failed\n");
    return 1;
  }
  if (!sampler.image) {
    logE("ScanlineDecoder::read: sampler.image must be non-NULL\n");
    return 1;
  }
  memory::Image& img = *sampler.image;
  if (!img.vk) {
    if (img.ctorAndBindDeviceLocal()) {
      logE("ScanlineDecoder::read: img.ctorAndBindDeviceLocal failed\n");
      return 1;
    }
  }
  if (!sampler.imageView.vk) {
    if (sampler.imageView.ctorError(img.vk, img.info.format)) {
      logE("ScanlineDecoder::read: sampler.imageView.ctorError failed\n");
      return 1;
    }
  }
  if (!sampler.vk) {
    if (sampler.ctorErrorNoImageViewInit()) {
      logE("ScanlineDecoder::read: sampler.ctorErrorNoImageViewInit failed\n");
      return 1;
    }
  }
  VkDeviceSize lines = stage.mmapMax() / cpu.stride;
  if (lines == 0) {
    logE("ScanlineDecoder::read: BUG: stride should be checked in open\n");
    return 1;
  }
  VkDeviceSize rem = img.info.extent.height - cpu.lineCount;
  if (rem < 1) {
    // Nothing left to read.
    return 0;
  }
  if (rem < lines) {
    lines = rem;
  }

  std::shared_ptr<memory::Flight> flight;
  if (stage.mmap(img, lines * cpu.stride, flight)) {
    logE("ScanlineDecoder::read: stage.mmap failed\n");
    return 1;
  }
  auto prevLineCount = cpu.lineCount;
  if (cpu.read(lines, flight->mmap())) {
    (void)stage.flushAndWait(flight);
    return 1;
  }

  flight->copies.resize(1);
  VkBufferImageCopy& copy = flight->copies.at(0);
  memset(&copy, 0, sizeof(copy));
  copy.bufferRowLength = img.info.extent.width;
  copy.bufferImageHeight = lines;
  copy.imageExtent = img.info.extent;
  copy.imageExtent.height = lines;
  copy.imageOffset.y = prevLineCount;
  copy.imageSubresource = img.getSubresourceLayers(0);
  copy.imageSubresource.mipLevel = dstMipLevel;
  copy.imageSubresource.baseArrayLayer = dstArrayLayer;
  copy.imageSubresource.layerCount = 1;

  if (stage.flushButNotSubmit(flight)) {
    logE("ScanlineDecoder::read: flushButNotSubmit failed\n");
    return 1;
  }
  if (cpu.lineCount >= img.info.extent.height) {
    // read is now done. Generate mipmaps. Submit all commands. Then close().
    if (!flight->canSubmit()) {
      if (img.info.mipLevels > 1) {
        // flight cannot be used for this. Make a new SmartCommandBuffer.
        science::SmartCommandBuffer smart{stage.pool, stage.poolQindex};
        if (smart.ctorError() || smart.autoSubmit() ||
            science::copyImageToMipmap(smart, img)) {
          logE("ScanlineDecoder::read: smart.copyImageToMipmap failed\n");
          return 1;
        }
      }
    } else {
      if (img.info.mipLevels > 1) {
        // Put a "generate mipmaps" command in flight before submitting it.
        if (science::copyImageToMipmap(*flight, img)) {
          logE("ScanlineDecoder::read: copyImageToMipmap failed\n");
          return 1;
        }
      }
      if (flight->end() ||
          stage.pool.submitAndWait(stage.poolQindex, *flight)) {
        logE("ScanlineDecoder::read: end or submitAndWait failed\n");
        return 1;
      }
    }
    flight.reset();
    if (close()) {
      logE("ScanlineDecoder::read: close failed\n");
      return 1;
    }
    return 0;
  }
  if (!flight->canSubmit()) {
    flight.reset();
    return 0;
  }

  // This is not the last read() call. canSubmit == true means flight must
  // still be submitted. Rather than block until submit is done, rely on the
  // caller to call read() again later so flight can run in the background.
  std::shared_ptr<command::Fence> fence = stage.pool.borrowFence();
  if (!fence) {
    logE("ScanlineDecoder::read: borrowFence failed\n");
    return 1;
  }
  command::CommandPool::lock_guard_t lock(stage.pool.lockmutex);
  if (flight->end() ||
      stage.pool.submit(lock, stage.poolQindex, *flight, fence->vk)) {
    (void)stage.pool.unborrowFence(fence);
    logE("ScanlineDecoder::read: submit failed\n");
    return 1;
  }
  inFlight.emplace_back(fence, flight);
  return 0;
}

int ScanlineDecoder::close() {
  if (flushFlights()) {
    logE("ScanlineDecoder::close: flushFlights failed\n");
    return 1;
  }
  return cpu.close();
}

int ScanlineDecoder::flushFlights() {
  // TODO: allow two flights - if needed.
  while (!inFlight.empty()) {
    FlightTracker& track = inFlight.at(0);
    auto fence = track.fence;
    auto flight = track.flight;
    inFlight.erase(inFlight.begin());
    VkResult v = fence->waitMs(1000);
    flight.reset();
    if (v != VK_SUCCESS) {
      (void)stage.pool.unborrowFence(fence);
      return explainVkResult("ScanlineDecoder::flushFlights: fence.waitMs", v);
    }
    if (stage.pool.unborrowFence(fence)) {
      logE("ScanlineDecoder::flushFlights: unborrowFence failed\n");
      return 1;
    }
  }
  return 0;
}
