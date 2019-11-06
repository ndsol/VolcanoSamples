/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 *
 * AssimpGlue class implementation
 */

#include "assimpglue.h"

#include <assimp/scene.h>
#include <string.h>

#include <assimp/DefaultLogger.hpp>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>

#include "base_application.h"

struct VolcanoGlueLogger : public Assimp::Logger {
  VolcanoGlueLogger() : Logger(), attachedStream(nullptr) {}
  virtual ~VolcanoGlueLogger() {
    if (attachedStream) {
      delete attachedStream;
    }
  }
  virtual bool attachStream(Assimp::LogStream* pStream,
                            unsigned int /*severity*/ = 0) {
    attachedStream = pStream;
    return true;
  }
  virtual bool detachStream(Assimp::LogStream* /*pStream*/,
                            unsigned int /*severity*/ = 0) {
    attachedStream = nullptr;
    return true;
  }
  Assimp::LogStream* attachedStream;

  virtual void OnVerboseDebug(const char* message) { logV("%s\n", message); }
  virtual void OnDebug(const char* message) { logD("%s\n", message); }
  virtual void OnInfo(const char* message) { logI("%s\n", message); }
  virtual void OnWarn(const char* message) { logW("%s\n", message); }
  virtual void OnError(const char* message) { logE("%s\n", message); }
};

#ifdef __ANDROID__
#include <android_native_app_glue.h>

struct VolcanoIOStream : public Assimp::IOStream {
  VolcanoIOStream(const char* name_, AAsset* asset_)
      : name(name_), asset(asset_), assetLength(AAsset_getLength64(asset_)) {}
  virtual ~VolcanoIOStream() {}
  std::string name;
  AAsset* asset{nullptr};
  off64_t assetLength{0};

  // FileSize returns the size of the asset.
  virtual size_t FileSize() const { return assetLength; }

  // Flush writes to disk any bytes that have been buffered (unsupported).
  virtual void Flush() {
    // Because no writing is supported, this function is a no-op.
  }

  // Read reads from the asset.
  virtual size_t Read(void* buf, size_t size, size_t count) {
    int r = AAsset_read(asset, buf, size * count);
    if (r < 0) {
      logE("Assimp::IOStream(%s): AAsset_read failed: %d %s\n", name.c_str(),
           errno, strerror(errno));
      return 0;
    }
    r /= size;
    return r;
  }

  // Seek seeks within the asset.
  virtual aiReturn Seek(size_t offset, aiOrigin origin) {
    static_assert(aiOrigin_CUR == SEEK_CUR && aiOrigin_END == SEEK_END &&
                      aiOrigin_SET == SEEK_SET,
                  "aiOrigin_{CUR,END,SET} == SEEK_*");
    switch (origin) {
      case aiOrigin_END:
        return (aiReturn)AAsset_seek64(asset, -((off64_t)offset), SEEK_END);
      case aiOrigin_SET:
      case aiOrigin_CUR:
        return (aiReturn)AAsset_seek64(asset, offset, origin);
      default:
        logF("Assimp::IOStream(%s)::Seek(%d) unsupported\n", name.c_str(),
             origin);
        return (aiReturn)-1;
    }
  }

  // Tell returns the current position of the read/write cursor.
  virtual size_t Tell() const {
    return (size_t)(assetLength - AAsset_getRemainingLength64(asset));
  }

  // Write writes to the asset (unsupported).
  virtual size_t Write(const void* /*buf*/, size_t /*size*/, size_t /*n*/) {
    return 0;
  }
};

struct VolcanoIOSystem : public Assimp::IOSystem {
  VolcanoIOSystem() {}
  virtual ~VolcanoIOSystem() {}

  // Assimp defines ComparePaths to be case insensitive, but the correct
  // implementation on Android is case sensitive.
  virtual bool ComparePaths(const char* a, const char* b) const {
    return strcmp(a, b);
  }

  // Exists tests for "file not found" errors.
  virtual bool Exists(const char* pFile) const {
    AAsset* a = AAssetManager_open(glfwGetAndroidApp()->activity->assetManager,
                                   pFile, AASSET_MODE_STREAMING);
    if (!a) {
      return false;
    }
    AAsset_close(a);
    return true;
  }

  // Open returns an IOStream ready to read the file.
  virtual Assimp::IOStream* Open(const char* pFile, const char* pMode = "rb") {
    if (strcmp(pMode, "r") && strcmp(pMode, "rb")) {
      logE("Assimp called Open(%s, pMode=%s): bad pMode\n", pFile, pMode);
      return nullptr;
    }

    AAsset* a = AAssetManager_open(glfwGetAndroidApp()->activity->assetManager,
                                   pFile, AASSET_MODE_STREAMING);
    if (!a) {
      logE("Assimp called Open(%s): AAssetManager failed (file not found?)\n",
           pFile);
      return nullptr;
    }
    return new VolcanoIOStream(pFile, a);
  }

  virtual void Close(Assimp::IOStream* pFile) {
    AAsset_close(dynamic_cast<VolcanoIOStream*>(pFile)->asset);
  }

  // getOsSeparator allows for windows backslashes.
  virtual char getOsSeparator() const { return '/'; }
};

#else /* not Android */

#include <assimp/DefaultIOSystem.h>

struct VolcanoIOSystem : public Assimp::DefaultIOSystem {
  virtual ~VolcanoIOSystem() {}

  // ComparePaths defaults to case insensitive. Fix to match posix.
  virtual bool ComparePaths(const char* a, const char* b) const {
    return strcmp(a, b);
  }

  // Exists tests for "file not found" errors.
  virtual bool Exists(const char* pFile) const {
    std::string found;  // found is discarded, only the return value is used.
    // findInPaths is defined in vendor/volcano/core/structs.h.
    return !findInPaths(pFile, found);
  }

  // Open returns an IOStream ready to read the file.
  virtual Assimp::IOStream* Open(const char* pFile, const char* pMode = "rb") {
    std::string found;
    // findInPaths is defined in vendor/volcano/core/structs.h.
    if (findInPaths(pFile, found)) {
      logE("Assimp::Open(%s, %s): not found\n", pFile, pMode);
      return nullptr;
    }
    return Assimp::DefaultIOSystem::Open(found.c_str(), pMode);
  }
};
#endif

int AssimpGlue::import(const char* fileName, const aiScene** outScene) {
  // Note: this doesn't have to get destroyed and recreated on every import,
  // but since VolcanoGlueLogger is stateless anyway, this is simpler.
  Assimp::DefaultLogger::set(new VolcanoGlueLogger());
  if (!importer) {
    importer.reset(new Assimp::Importer());
    importer->SetIOHandler(new VolcanoIOSystem());
  }

  const aiScene* scene = importer->ReadFile(fileName, fileMode);
  if (!scene) {
    logE("Assimp::Importer::ReadFile(%s) failed\n", fileName);
    return 1;
  }
  if (scene->mNumMeshes < 1) {
    logE("Assimp::Importer::ReadFile(%s): no meshes in file\n", fileName);
    return 1;
  }
  *outScene = scene;
#if 0
#include <assimp/Exporter.hpp>
  Assimp::Exporter* exporter = new Assimp::Exporter();
  size_t n = exporter->GetExportFormatCount();
  for (size_t i = 0; i < n; i++) {
    auto* d = exporter->GetExportFormatDescription(i);
    logW("%zu/%zu: ext=.%s id=\"%s\"\n", i, n, d->fileExtension, d->id);
  }
  std::string exportFileName = fileName;
  exportFileName += ".assbin";
  auto r = exporter->Export(scene, "assbin", exportFileName);
  if (r == AI_SUCCESS) {
    logE("Export: AI_SUCCESS\n");
  } else {
    logE("Export: %d\n", r);
    return 1;
  }
#endif
  return 0;
}
