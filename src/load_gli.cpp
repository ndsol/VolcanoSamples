/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 */
#include "load_gli.h"

int loadGLI(const char* textureFileName, std::string& textureFound,
            gli::texture& out) {
#ifdef __ANDROID__
  AAsset* asset =
      AAssetManager_open(glfwGetAndroidApp()->activity->assetManager,
                         textureFileName, AASSET_MODE_STREAMING);
  if (!asset) {
    logE("AAssetManager_open(%s) failed\n", textureFileName);
    return 1;
  }
  off64_t size = AAsset_getLength64(asset);
  if (!size) {
    logE("AAsset_getLength64(%s) returned 0\n", textureFileName);
    return 1;
  }
  char* data = new char[size];
  if (!data) {
    logE("new char[%lld] failed\n", (long long)size);
    return 1;
  }
  int r = AAsset_read(asset, data, size);
  if (r < 0) {
    logE("AAsset_read(%s) failed: %d %s\n", textureFileName, errno,
         strerror(errno));
    return 1;
  }
  AAsset_close(asset);
  textureFound = textureFileName;
  out = gli::load(data, size);
  delete[] data;
#else /* not __ANDROID__ */
  if (findInPaths(textureFileName, textureFound)) {
    return 1;
  }
  out = gli::load(textureFound.c_str());
#endif
  return 0;
}

int constructSamplerGLI(science::Sampler& sampler, memory::Stage& stage,
                        gli::texture& src) {
  if (src.format() > gli::FORMAT_RGBA_ASTC_12X12_SRGB_BLOCK16) {
    logE("constructSamplerGLI: format %u needs a custom conversion to vulkan\n",
         static_cast<unsigned>(src.format()));
    return 1;
  }
  auto& info = sampler.image->info;
  sampler.imageView.info.subresourceRange.levelCount = sampler.info.maxLod;
  info.mipLevels = sampler.info.maxLod;
  uint32_t formatBytes = gli::block_size(src.format());
  info.format = static_cast<VkFormat>(src.format());
  info.extent.width = src.extent().x;
  info.extent.height = src.extent().y;
  info.extent.depth = 1;

  if (sampler.ctorError()) {
    logE("constructSampler: sampler.ctorError failed\n");
    return 1;
  }

  std::shared_ptr<memory::Flight> flight;
  if (stage.mmap(*sampler.image, src.size(), flight)) {
    logE("constructSampler: src.size=%zu stage.mmap failed\n",
         (size_t)src.size());
    return 1;
  }
  memcpy(flight->mmap(), src.data<char>(), src.size());

  flight->copies.resize(info.mipLevels);
  size_t readOfs = 0;
  for (size_t mip = 0; mip < info.mipLevels; mip++) {
    VkBufferImageCopy& copy = flight->copies.at(mip);
    copy.bufferOffset = readOfs;
    copy.bufferRowLength = std::max(1u, info.extent.width >> mip);
    copy.bufferImageHeight = std::max(1u, info.extent.height >> mip);
    readOfs += formatBytes * copy.bufferRowLength * copy.bufferImageHeight;
    copy.imageSubresource = sampler.image->getSubresourceLayers(mip);
    copy.imageOffset = {0, 0, 0};
    copy.imageExtent = {copy.bufferRowLength, copy.bufferImageHeight, 1};
  }
  if (stage.flushAndWait(flight)) {
    logE("constructSampler: flushAndWait failed\n");
    return 1;
  }
  science::SmartCommandBuffer smart(stage.pool, stage.poolQindex);
  if (smart.ctorError() || smart.autoSubmit() ||
      smart.barrier(*sampler.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
    logE("barrier(SHADER_READ_ONLY) failed\n");
    return 1;
  }
  return 0;
}
