/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 */
#include "base_application.h"

BaseApplication::~BaseApplication(){};

// Work around <chrono> bug: if high_resolution_clock::time_point() is used
// here, all calls to high_resolution_clock::now() return a constant, the
// time the program was compiled.
static auto programStart = std::chrono::high_resolution_clock::now();

float Timer::now() {
  return std::chrono::duration<float>(
             std::chrono::high_resolution_clock::now() - programStart)
      .count();
}
