/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * RetroGLFW provides a bare-bones GLFW UI for RetroWeb.
 *
 * One piece of code for RetroGLFW must be added at a very specific place in
 * your app: androidSetNeedsMenuKey(1) must be the first thing in
 * your crossPlatformMain().
 */

#include "retroglfw.h"

#include "imgui.h"
#include "include/libretro.h"

#ifdef __ANDROID__
#include <android/keycodes.h>
#endif /*__ANDROID__*/

const std::map<int, int> toRetroKey = {
#define MAP(a) {GLFW_KEY_##a, RETROK_##a},
#define MAP2(a, b) {GLFW_KEY_##a, RETROK_##b},
    // clang-format off
    MAP(TAB) MAP(PAUSE) MAP(ESCAPE) MAP(SPACE) MAP(BACKSLASH)
    MAP(BACKSPACE) MAP(DELETE) MAP(KP_ENTER) MAP2(KP_EQUAL, KP_EQUALS)
    MAP2(KP_DECIMAL, KP_PERIOD) MAP(KP_DIVIDE) MAP(KP_MULTIPLY)
    MAP2(KP_SUBTRACT, KP_MINUS) MAP2(KP_ADD, KP_PLUS)
    MAP(UP) MAP(DOWN) MAP(RIGHT) MAP(LEFT)
    MAP(INSERT) MAP(HOME) MAP(END) MAP(MENU)
    MAP2(PAGE_UP, PAGEUP) MAP2(PAGE_DOWN, PAGEDOWN)
    MAP2(RIGHT_SHIFT, RSHIFT)  MAP2(LEFT_SHIFT, LSHIFT)
    MAP2(RIGHT_CONTROL, RCTRL) MAP2(LEFT_CONTROL, LCTRL)
    MAP2(RIGHT_ALT, RALT)      MAP2(LEFT_ALT, LALT)
    MAP2(RIGHT_SUPER, RSUPER)  MAP2(LEFT_SUPER, LSUPER)
    MAP2(PRINT_SCREEN, SYSREQ) MAP2(GRAVE_ACCENT, BACKQUOTE)
    MAP2(CAPS_LOCK, CAPSLOCK)  MAP2(SCROLL_LOCK, SCROLLOCK)
    MAP2(NUM_LOCK, NUMLOCK) MAP2(APOSTROPHE, QUOTE) MAP2(ENTER, RETURN)
    MAP2(LEFT_BRACKET, LEFTBRACKET) MAP2(RIGHT_BRACKET, RIGHTBRACKET)
    MAP(COMMA) MAP(MINUS) MAP(PERIOD) MAP(SLASH) MAP(SEMICOLON)
    MAP(0) MAP(1) MAP(2) MAP(3) MAP(4) MAP2(EQUAL, EQUALS)
    MAP(5) MAP(6) MAP(7) MAP(8) MAP(9)
    MAP2(A, a) MAP2(B, b) MAP2(C, c) MAP2(D, d) MAP2(E, e)
    MAP2(F, f) MAP2(G, g) MAP2(H, h) MAP2(I, i) MAP2(J, j)
    MAP2(K, k) MAP2(L, l) MAP2(M, m) MAP2(N, n) MAP2(O, o)
    MAP2(P, p) MAP2(Q, q) MAP2(R, r) MAP2(S, s) MAP2(T, t)
    MAP2(U, u) MAP2(V, v) MAP2(W, w) MAP2(X, x) MAP2(Y, y)
    MAP2(Z, z) MAP(F1) MAP(F2) MAP(F3) MAP(F4) MAP(F5) MAP(F6) MAP(F7)
    MAP(F8) MAP(F9) MAP(F10) MAP(F11) MAP(F12) MAP(F13) MAP(F14) MAP(F15)
    // The following RETROK_* codes cannot be sent from GLFW:
    //BREAK
    //CLEAR
    //EXCLAIM
    //QUOTEDBL
    //HASH
    //DOLLAR
    //AMPERSAND
    //LEFTPAREN
    //RIGHTPAREN
    //ASTERISK
    //PLUS
    //COLON
    //LESS
    //GREATER
    //QUESTION
    //AT
    //CARET
    //UNDERSCORE
    //LEFTBRACE
    //BAR
    //RIGHTBRACE
    //TILDE
    //KP0 through KP9
    //RMETA
    //LMETA
    //MODE
    //COMPOSE
    //HELP
    //PRINT
    //POWER
    //EURO
    //UNDO
// clang-format on
#undef MAP
#undef MAP2
};

RetroGLFW::RetroGLFW(UniformGlue& uglue, RetroWeb& retroweb)
    : uglue(uglue), retroweb(retroweb) {
  setUserFreeResize(false);
  uglue.inputEventListeners.push_back(std::make_pair(
      [](void* self, GLFWinputEvent* e, size_t eCount, int m,
         int enter) -> void {
        static_cast<RetroGLFW*>(self)->onInputEvent(e, eCount, m, enter);
      },
      this));
  uglue.keyEventListeners.push_back(std::make_pair(
      [](void* self, int key, int /*scancode*/, int action, int mods) -> void {
        auto* me = static_cast<RetroGLFW*>(self);
        if (me->isSpecialKey(key, action, mods)) {
          return;
        }
        if (ImGui::GetIO().WantCaptureKeyboard ||
            me->curApp == me->retroweb.getApps().end()) {
          return;  // No keyboard events sent to core if WantCaptureKeyboard.
        }
        auto i = toRetroKey.find(key);
        if (i == toRetroKey.end()) {
          return;
        }
        me->curApp->second.core.keyState.at(i->second) = !!action;
        me->lastMods = mods;
      },
      this));
#ifdef __ANDROID__
  imGuiEatsGamepad = true;
#endif /*__ANDROID__*/
}

int RetroGLFW::isSpecialKey(int key, int action, int mods) {
  (void)mods;
  switch (key) {
    case GLFW_KEY_P:  // GLFW_KEY_P is mostly for desktop which has a keyboard.
    case GLFW_KEY_F17:
    case GLFW_KEY_MENU:
      // GLFW_KEY_MENU is mostly for Android, i.e. no keyboard, but uses
      // androidSetNeedsMenuKey.
      if (action == GLFW_PRESS) {
        if (gamePaused()) {
          doUnpause();
        } else {
          doPause();
        }
      }
      break;
    case GLFW_KEY_F2:
      if (action == GLFW_PRESS && curApp != retroweb.getApps().end()) {
        std::string name = "autosave";
        if (retroweb.saveTo(curApp->second, name)) {
          // Do nothing on failure.
          if (curApp->second.core.isLastSaveTooEarly()) {
            // Could try saving again later.
          }
        }
      }
      break;
    case GLFW_KEY_F3:
      if (action == GLFW_PRESS && curApp != retroweb.getApps().end()) {
        std::string wantApp;
        std::string name = "autosave";
        if (retroweb.loadSave(curApp, name, wantApp)) {
          // Do nothing on failure.
        }
      }
      break;
    case GLFW_KEY_F4:
      if (action == GLFW_PRESS && curApp != retroweb.getApps().end()) {
        if (curApp->second.core.getName() == "nestopia") {
          curApp->second.core.setVarToNext("nestopia_palette");
        }
      }
      break;
    default:
      return 0;
  }
  return 1;
}

void RetroGLFW::onInputEvent(GLFWinputEvent* events, size_t eventCount,
                             int mods, int entered) {
  lastMods = mods;
  if (lastFocused != uglue.focused) {
    if (!uglue.focused && !gamePaused()) {
      doPause();
    }
    lastFocused = uglue.focused;
  }
#ifdef __ANDROID__
  for (size_t i = 0; i < eventCount; i++) {
    auto& ev = events[i];
    if (ev.inputDevice == GLFW_INPUT_JOYSTICK) {
      if (!glfwJoystickPresent(ev.num)) {
        continue;
      }
      int count;
      const unsigned char* b = glfwGetJoystickButtons(ev.num, &count);
      if (count > AKEYCODE_BACK) {
        if (prevBackButton != !!b[AKEYCODE_BACK]) {
          (void)isSpecialKey(GLFW_KEY_MENU,
                             !!b[AKEYCODE_BACK] ? GLFW_PRESS : GLFW_RELEASE, 0);
        }
        prevBackButton = !!b[AKEYCODE_BACK];
      }
    }
  }
#endif
  if (curApp == retroweb.getApps().end()) {
    return;
  }
  auto& core = curApp->second.core;
  if (ImGui::GetIO().WantCaptureMouse) {
    core.mouseEnter = 0;
    return;  // No mouse or touch events sent to core if WantCaptureMouse.
  }
  int winW, winH;
  glfwGetWindowSize(uglue.window, &winW, &winH);

  core.mouseEnter = entered;
  core.mouseButton = 0;
  for (size_t i = 0; i < eventCount; i++) {
    auto& ev = events[i];
    if (ev.inputDevice == GLFW_INPUT_JOYSTICK) {
      continue;  // Poll joysticks with glfwGetJoystickButtons() instead.
    }
    if (ev.inputDevice == GLFW_INPUT_FINGER && !ev.buttons) {
      continue;  // "Finger just vanished" events just leave mouseButton at 0.
    }
    core.mouseButton = !!ev.buttons;
    core.mouseX = ev.x * 0x7fff * 2 / winW - 0x7fff;
    core.mouseY = ev.y * 0x7fff * 2 / winH - 0x7fff;
  }
}

int RetroGLFW::updateImGuiInputs() {
  if (retroweb.poll()) {
    logE("updateImGuiInputs: retroweb.poll() failed\n");
    return 1;
  }
  auto& io = ImGui::GetIO();
  if (!(io.BackendFlags & ImGuiBackendFlags_HasGamepad) ||
      (imGuiEatsGamepad && mainWindowOpen)) {
    return 0;
  }

  // Send gamepad inputs to current active core, not Dear ImGui.
  memset(io.NavInputs, 0, sizeof(io.NavInputs));
  if (curApp == retroweb.getApps().end()) {
    return 0;
  }
  for (int jid = 0; jid <= GLFW_JOYSTICK_LAST; jid++) {
    if (!glfwJoystickPresent(jid)) {
      continue;
    }
    std::vector<std::pair<unsigned, int>> state;
    int bCount;
    const unsigned char* b = glfwGetJoystickButtons(jid, &bCount);
#ifdef __ANDROID__
    static constexpr int maxButton = AKEYCODE_BUTTON_R1;
    static constexpr int maxAxis = 11;
#else  /*__ANDROID__*/
    static constexpr int maxButton = 2;
    static constexpr int maxAxis = 1;
#endif /*__ANDROID__*/
    if (bCount <= maxButton) {
      continue;
    }
    int count;
    const float* axes = glfwGetJoystickAxes(jid, &count);
    if (count <= maxAxis) {
      continue;
    }
    static constexpr float axisDeadZone = .1f;
#define mapButton(c, d) \
  if (d < bCount) state.emplace_back(RETRO_DEVICE_ID_JOYPAD_##c, b[d] ? 1 : 0)
#define mapAxisBtn(c, d, scale)                  \
  state.emplace_back(RETRO_DEVICE_ID_JOYPAD_##c, \
                     ((scale)*axes[d]) > axisDeadZone ? 1 : 0)
#ifdef __ANDROID__
    mapButton(A, AKEYCODE_BUTTON_A);
    mapButton(B, AKEYCODE_BUTTON_B);
    mapButton(X, AKEYCODE_BUTTON_X);
    mapButton(Y, AKEYCODE_BUTTON_Y);
    mapButton(L, AKEYCODE_BUTTON_L1);
    mapButton(R, AKEYCODE_BUTTON_R1);
    // mapButton(SELECT, ???);
    state.emplace_back(
        RETRO_DEVICE_ID_JOYPAD_START,
        (b[AKEYCODE_BUTTON_START] | b[AKEYCODE_MEDIA_PLAY_PAUSE]) ? 1 : 0);
    mapAxisBtn(UP, 11, -1);  // Axes 10, 11 are Left D-pad
    mapAxisBtn(DOWN, 11, 1);
    mapAxisBtn(LEFT, 10, -1);
    mapAxisBtn(RIGHT, 10, 1);
#else  /*__ANDROID__*/
    mapButton(A, 0);
    mapButton(B, 1);
    mapButton(X, 2);
    mapButton(Y, 3);
    mapButton(L, 4);
    mapButton(R, 5);
    mapButton(START, 6);
    mapAxisBtn(UP, 1, -1);
    if (bCount > 12 && b[12]) state.back().second = 1;
    mapAxisBtn(DOWN, 1, 1);
    if (bCount > 14 && b[14]) state.back().second = 1;
    mapAxisBtn(LEFT, 0, -1);
    if (bCount > 15 && b[15]) state.back().second = 1;
    mapAxisBtn(RIGHT, 0, 1);
    if (bCount > 13 && b[13]) state.back().second = 1;
#endif /*__ANDROID__*/
#undef mapButton
#undef mapAxisBtn
    auto& ports = curApp->second.core.ports;
    for (auto i = ports.begin(); i != ports.end(); i++) {
      if (i->second.device != RETRO_DEVICE_JOYPAD) {
        continue;
      }
      auto port = i->first;
      for (auto p : state) {
        if (curApp->second.core.updateJoypad(port, p.first, p.second)) {
          logE("%s updateJoypad(%u, %d) failed\n",
               curApp->second.getName().c_str(), p.first, p.second);
        }
      }
      // Tell curApp not to translate keyboard keys into gamepad inputs.
      curApp->second.core.mapKeysToJoypad = false;
    }
  }
  return 0;
}

void RetroGLFW::setUserFreeResize(bool want) {
#ifdef __ANDROID__
  userFreeResize = true;  // Android screen is not under user control.
#else                     /*__ANDROID__*/
  userFreeResize = want;
  if (want || wantEx.width < 1 || wantEx.height < 1 ||
      curApp == retroweb.getApps().end()) {
    glfwSetWindowAspectRatio(uglue.window, GLFW_DONT_CARE, GLFW_DONT_CARE);
  } else {
    needFitToAspect = true;
  }
  setUBOdirtyAll();  // Rebuild UBO with new content size.
#endif                    /*__ANDROID__*/
}

void RetroGLFW::doPause() {
  mainWindowOpen = true;
  pauseButRender = 2;
}

void RetroGLFW::doUnpause() {
  mainWindowOpen = false;
  ImGui::SetWindowFocus(NULL);
  pauseButRender = 0;
}
