/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 *
 * The BaseApplication class abstracts OS-specific application setup. See
 * https://github.com/ndsol/VolcanoSamples/tree/master/01glfw
 */

#pragma once

#include <src/science/science.h>

class BaseApplication : public science::CommandPoolContainer {
 public:
  // instance is the Vulkan instance.
  language::Instance& instance;

  BaseApplication(language::Instance& instance)
      : CommandPoolContainer(*instance.devs.at(0)), instance(instance) {}
  virtual ~BaseApplication();
};

struct Timer {
  Timer() { reset(); }
  // reset changes startTime so that get() starts over with the current time.
  void reset() { startTime = Timer::now(); }
  float get() const { return Timer::now() - startTime; }
  // now() returns a good clock for GUI applications and animations.
  static float now();

  // startTime holds the value of now() when the last reset() was called.
  float startTime;
};
