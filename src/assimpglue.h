/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 */

#pragma once

#include <assimp/postprocess.h>

#include <assimp/Importer.hpp>
#include <memory>

class BaseApplication;

class AssimpGlue {
 protected:
  std::shared_ptr<Assimp::Importer> importer;

 public:
  unsigned fileMode =
      ::aiProcess_Triangulate | ::aiProcess_PreTransformVertices |
      ::aiProcess_CalcTangentSpace | ::aiProcess_GenSmoothNormals;

  // import imports an asset using assimp, and returns a pointer to an aiScene.
  // Assimp retains ownership of the pointer, and the pointer is invalid after
  // AssimpGlue::reset().
  int import(const char* fileName, const aiScene** outScene);

  // reset frees any memory used by the Assimp::Importer. This can optionally be
  // called when the application is done reading all assets to reduce mem use.
  //
  // Any aiScene* outScene pointers are invalid after reset returns.
  void reset() { importer.reset(); }
};
