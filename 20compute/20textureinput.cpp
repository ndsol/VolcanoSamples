/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * Implements reading an input file with help from Skia
 */

#include "20texture.h"

namespace example {

int Compressor::handleSkiaError(SkCodec::Result r, const char* filename) {
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

}  // namespace example
