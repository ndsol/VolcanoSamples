/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 */

#pragma once

#ifdef __ANDROID__
#include <android_native_app_glue.h>
#endif /*__ANDROID__*/
#include <src/science/science.h>

#include <gli/gli.hpp>
#include <string>

// loadGLI finds textureFileName and loads it into gli::texture& out.
// textureFound is set to the path and file name that was used.
int loadGLI(const char* textureFileName, std::string& textureFound,
            gli::texture& out);

// constructSampler is a convenience function that builds a memory::Sampler
// from a gli::texture.
//
// constructSampler expects sampler.info.maxLod to be set to the desired
// number of mip levels and ignores src.levels().
int constructSamplerGLI(science::Sampler& sampler, memory::Stage& stage,
                        gli::texture& src);
