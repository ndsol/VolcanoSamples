/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * RetroGLFW provides a bare-bones GLFW UI for RetroWeb.
 *
 * One piece of code for RetroGLFW must be added at a very specific place in
 * your app: androidSetNeedsMenuKey(1) must be the first thing in
 * your crossPlatformMain().
 */

#pragma once

#include <map>

#include "../../src/uniformglue/uniformglue.h"
#include "retroweb.h"

class RetroGLFW {
 public:
  RetroGLFW(UniformGlue& uglue, RetroWeb& retroweb);

  UniformGlue& uglue;
  RetroWeb& retroweb;
  int lastMods{0};
  int lastFocused{0};
  // GLFW lock aspect ratio (if the user requests it)
  bool userFreeResize;
  bool needFitToAspect{false};
  // wantEx is the extent (size) requested by the core.
  VkExtent2D wantEx{0, 0};
  // emuFrameFmt is the VkFormat requested by the core.
  VkFormat emuFrameFmt;
  // emuRate is the sample rate requested by the core.
  int emuRate{0};
  bool mainWindowOpen{true};
  // pauseButRender runs the core for a set number of frames.
  int pauseButRender{0};
  bool gamePaused() const { return mainWindowOpen; }
  void doPause();
  void doUnpause();
  // imGuiEatsGamepad is for the ambiguous situation:
  // * Detecting if a gamepad is present can be uncertain. Are you *sure* you
  //   have found one?
  // * Then, should the gamepad be listened to, and not the keyboard?
  // The answer is somewhat OS-dependent, but your app can also overwrite the
  // setting by changing this value.
  bool imGuiEatsGamepad{false};
#ifdef __ANDROID__
  bool prevBackButton{false};
#endif /*__ANDROID__*/
  float progressFraction{0.f};
  mapStringToApp::iterator curApp = retroweb.getApps().end();

  // isSpecialKey returns 1 to swallow a key event before RetroArch sees it.
  virtual int isSpecialKey(int key, int action, int mods);

  // onInputEvent receives events for keyboard, touch, mouse and joysticks.
  virtual void onInputEvent(GLFWinputEvent* events, size_t eventCount, int mods,
                            int /*entered*/);

  // updateImGuiInputs must be called before ImGui::NewFrame()
  int updateImGuiInputs();

  bool isUBOdirty(uint32_t next_image_i) {
    if (next_image_i > 30) {
      logE("uboDirty is a bitfield. next_image_i=%u will overflow\n",
           next_image_i);
      return true;
    }
    return uboDirty & (1u << next_image_i);
  }

  void setUBOdirty(uint32_t next_image_i, int dirty) {
    if (dirty) {
      uboDirty |= 1u << next_image_i;
    } else {
      uboDirty &= ~(1u << next_image_i);
    }
  }

  void setUBOdirtyAll() { uboDirty = (uint32_t)-1; }

  void setUserFreeResize(bool want);

 protected:
  // uboDirty tracks if a given UBO can be used without updating it.
  uint32_t uboDirty{(uint32_t)-1};
};
