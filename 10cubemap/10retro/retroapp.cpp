/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * RetroApp implementation.
 */

#include "include/libretro.h"
#include "retrocore.h"

RetroApp::~RetroApp() {
  if (running) {
    close();
  }
}

int RetroApp::open() {
  if (romPath.empty() && !core.getNoRom()) {
    logE("open: core \"%s\" needs a rom\n", core.getCorepath().c_str());
    return 1;
  }

  if (!core.getNoRom() && core.setRomPath(romPath, romSize)) {
    logE("open: setRomPath(%s) failed\n", romPath.c_str());
    return 1;
  }
  if (core.launchRom()) {
    logE("open: core \"%s\" launchRom failed\n", core.getCorepath().c_str());
    return 1;
  }
  running = true;
  return 0;
}

int RetroApp::close() {
  if (!running) {
    logE("RetroApp[%s]::close: not running\n", name.c_str());
    return 1;
  }
  running = false;
  return 0;
}

int RetroApp::nextFrame(RetroUIinterface& ui) {
  if (!running) {
    logE("RetroApp[%s]::nextFrame: must call open first\n", name.c_str());
    return 1;
  }
  return core.nextFrame(ui);
}

int RetroApp::load(const std::string& pathname, off_t filesize,
                   const char* filename) {
  romPath = pathname;
  romSize = filesize;
  name = filename;
  return 0;
}
