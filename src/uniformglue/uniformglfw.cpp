/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * Implements the GLFW logic for class UniformGlue.
 */

#include <src/core/utf8enc.h>

#include "imgui.h"
#include "uniformglue.h"
#ifdef __ANDROID__
#include <android/keycodes.h>
#endif /*__ANDROID__*/

void applyPreTransform(BaseApplication& app, double& x, double& y) {
  auto& ex = app.cpool.vk.dev.swapChainInfo.imageExtent;
  switch (app.cpool.vk.dev.swapChainInfo.preTransform) {
    case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
    case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR:
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR:
      // Rotated dimensions
      x = (x * ex.width) / ex.height;
      y = (y * ex.height) / ex.width;
      break;
    default:
      // Normal dimensions
      break;
  }
}

void UniformGlue::processInputEvents(GLFWinputEvent* events, int eventCount,
                                     int mods) {
  if (isImguiAvailable()) {
    auto& io = ImGui::GetIO();
    static constexpr size_t MouseDownSize =
        sizeof(io.MouseDown) / sizeof(io.MouseDown[0]);

    io.MouseWheel = 0;
    size_t i;
    for (i = 0; i < MouseDownSize; i++) {
      if (!((fastButtons | curFrameButtons) & (1u << i))) {
        io.MouseDown[i] = false;
      }
    }

    if (prevEntered) {
      for (i = 0; i < (size_t)eventCount; i++) {
        if (events[i].inputDevice == GLFW_INPUT_JOYSTICK) {
          continue;
        }

        io.MouseWheel += events[i].yoffset;
        for (size_t j = 0; j < MouseDownSize; j++) {
          unsigned bit = 1u << j;
          if (!(events[i].buttons & bit) && (curFrameButtons & bit)) {
            // Sometimes the GLFW_RELEASE comes in a second call to
            // processInputEvents() but during the same frame - a fast button.
            fastButtons |= bit;
          } else {
            // This updates the MouseDown boolean. It can set it to true, but
            // it will only *clear* it if this is not a fastButton.
            io.MouseDown[j] = (events[i].buttons & bit) != 0;
          }
          curFrameButtons |= events[i].buttons & bit;
        }

        if (events[i].inputDevice != GLFW_INPUT_FINGER) {
          // Button 2, 3, and higher are a "right click" (ignore finger inputs)
          io.MouseDown[1] |= (events[0].buttons & (~1)) != 0;
        }
        // Invert rot to match ImGui (leave app to rotate itself if desired).
        glm::mat2 rot;
        glm::vec2 translate;
        getImGuiRotation(rot, translate);
        rot = glm::inverse(rot);
        // Recalculate translate.xy using inverted rot.
        auto& imageExtent = app.cpool.vk.dev.swapChainInfo.imageExtent;
        translate.x = (rot[0].x < 0 || rot[0].y < 0) ? imageExtent.width : 0;
        translate.y = (rot[1].x < 0 || rot[1].y < 0) ? imageExtent.height : 0;

        auto& io = ImGui::GetIO();
        translate.x /= io.DisplayFramebufferScale.x;
        translate.y /= io.DisplayFramebufferScale.y;

        glm::dvec2 p = glm::vec2(events[i].x, events[i].y) * rot + translate;
        glm::dvec2 q(1., 1.);
        applyPreTransform(app, q.x, q.y);
        io.MousePos.x = (decltype(io.MousePos.x))(p.x / q.x);
        io.MousePos.y = (decltype(io.MousePos.y))(p.y / q.y);
      }
    }

    if (io.WantCaptureMouse) {
      return;
    }
  }

  for (size_t i = 0; i < (size_t)eventCount; i++) {
    applyPreTransform(app, events[i].x, events[i].y);
  }

  for (auto& p : inputEventListeners) {
    p.first(p.second, events, (size_t)eventCount, mods, prevEntered);
  }
  prevInput.assign(events, &events[eventCount]);
}

void UniformGlue::onGLFWResized(GLFWwindow* window, int w, int h) {
  auto self = reinterpret_cast<UniformGlue*>(glfwGetWindowUserPointer(window));
  if (w == 0 || h == 0) {
    // Window was minimized or moved offscreen.
    return;
  }
  uint32_t width = w, height = h;
  if (self->app.onResized({width, height}, memory::ASSUME_POOL_QINDEX)) {
    logE("onGLFWResized: onResized failed!\n");
    glfwSetWindowShouldClose(window, GLFW_TRUE);
    return;
  }
  self->needRebuild = false;
  onGLFWRefresh(window);
}

void UniformGlue::onGLFWRefresh(GLFWwindow* window) {
  auto self = reinterpret_cast<UniformGlue*>(glfwGetWindowUserPointer(window));
  if (self->inGLFWRefresh) {
    logE("onGLFWRefresh called while already executing on the call stack.\n");
    logE("This is a bug! Your app may need to defer window-related calls.\n");
    self->redrawErrorCount++;
    return;
  }
  self->inGLFWRefresh = true;
  if (self->onGLFWRefreshError()) {
    self->redrawErrorCount++;
  }
  self->inGLFWRefresh = false;
}

static void mapButton(ImGuiIO& io, const unsigned char* b, int mapFrom,
                      ImGuiNavInput_ mapTo) {
  io.NavInputs[mapTo] = b[mapFrom] ? 1.f : 0.f;
}

static constexpr float axisDeadZone = .1f;
static void mapAxis(ImGuiIO& io, const float* axes, int mapFrom,
                    ImGuiNavInput_ mapTo, float scale) {
  float val = axes[mapFrom] * scale;
  if (val > axisDeadZone) {
    io.NavInputs[mapTo] = val > 1.f ? 1.f : val;
  }
}

static void mapAxisUp(ImGuiIO& io, const float* axes, int mapFrom,
                      ImGuiNavInput_ mapTo) {
  mapAxis(io, axes, mapFrom, mapTo, -1.f);
}

static void mapAxisDn(ImGuiIO& io, const float* axes, int mapFrom,
                      ImGuiNavInput_ mapTo) {
  mapAxis(io, axes, mapFrom, mapTo, 1.f);
}

int UniformGlue::onGLFWRefreshError() {
  // Update GLFW joystick state here, not in onInputEvent, because Dear Imgui
  // resets button state to 0 in EndFrame(). That means it has to be polled.
  curJoyX = 0;
  curJoyY = 0;
  if (isImguiAvailable()) {
    auto& io = ImGui::GetIO();
    memset(io.NavInputs, 0, sizeof(io.NavInputs));
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    for (int jid = 0; jid <= GLFW_JOYSTICK_LAST; jid++) {
      if (!glfwJoystickPresent(jid)) {
        continue;
      }
      int count;
      const unsigned char* b = glfwGetJoystickButtons(jid, &count);
#ifdef __ANDROID__
      static constexpr int maxButton = AKEYCODE_BUTTON_R1;
#else  /*__ANDROID__*/
      static constexpr int maxButton = 5;
#endif /*__ANDROID__*/
      if (count <= maxButton) {
        continue;
      }
      const float* axes = glfwGetJoystickAxes(jid, &count);
      io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
      if (count <= 11) {
        continue;
      }
      curJoyX += (fabsf(axes[2]) < axisDeadZone) ? 0 : axes[2];
      curJoyY += (fabsf(axes[3]) < axisDeadZone) ? 0 : axes[3];
      io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
      io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

      // Found a usable Android Joypad
#ifdef __ANDROID__
      mapButton(io, b, AKEYCODE_BUTTON_A, ImGuiNavInput_Activate);
      mapButton(io, b, AKEYCODE_BUTTON_B, ImGuiNavInput_Cancel);
      mapButton(io, b, AKEYCODE_BUTTON_X, ImGuiNavInput_Input);
      mapButton(io, b, AKEYCODE_BUTTON_Y, ImGuiNavInput_Menu);
      mapButton(io, b, AKEYCODE_BUTTON_L1, ImGuiNavInput_FocusPrev);
      mapButton(io, b, AKEYCODE_BUTTON_R1, ImGuiNavInput_TweakSlow);
#else                                                    /*__ANDROID__*/
      mapButton(io, b, 0, ImGuiNavInput_Activate);
      mapButton(io, b, 1, ImGuiNavInput_Cancel);
      mapButton(io, b, 2, ImGuiNavInput_Input);
      mapButton(io, b, 3, ImGuiNavInput_Menu);
      mapButton(io, b, 4, ImGuiNavInput_FocusPrev);
      mapButton(io, b, 5, ImGuiNavInput_TweakSlow);
#endif                                                   /*__ANDROID__*/
      mapAxisUp(io, axes, 10, ImGuiNavInput_DpadLeft);   // Axes 10, 11 are
      mapAxisDn(io, axes, 10, ImGuiNavInput_DpadRight);  // Left D-pad
      mapAxisUp(io, axes, 11, ImGuiNavInput_DpadUp);
      mapAxisDn(io, axes, 11, ImGuiNavInput_DpadDown);
      mapAxisUp(io, axes, 0, ImGuiNavInput_LStickLeft);   // Axes 0, 1 are
      mapAxisDn(io, axes, 0, ImGuiNavInput_LStickRight);  // L analog stick
      mapAxisUp(io, axes, 1, ImGuiNavInput_LStickUp);
      mapAxisDn(io, axes, 1, ImGuiNavInput_LStickDown);
      mapAxisUp(io, axes, 5, ImGuiNavInput_FocusNext);  // L analog trigger
      mapAxisUp(io, axes, 4, ImGuiNavInput_TweakFast);  // R analog trigger
    }
  }

  std::shared_ptr<memory::Flight> flight;
  if (acquire()) {
    return 1;
  } else if (isAborted()) {
    return 0;
  } else if (stage.mmap(uniform.at(nextImage), 0, uboSize, flight)) {
    logE("UniformGlue::onGLFWRefresh: stage.mmap failed\n");
    return 1;
  }
  for (auto& p : redrawListeners) {
    if (p.first(p.second, flight)) {
      return 1;
    }
    if (isAborted()) {
      return 0;
    }
  }
#ifdef __ANDROID__
  if (isImguiAvailable()) {
    auto& io = ImGui::GetIO();
    for (auto i = keyTooFast.begin(); i != keyTooFast.end();) {
      if (!i->second) {  // GLFW_PRESS but not GLFW_RELEASE. Skip this one.
        i++;
      } else {  // Tell ImGui about the GLFW_RELEASE and erase it.
        io.KeysDown[i->first] = 0;
        i = keyTooFast.erase(i);
      }
    }
  }
#endif /*__ANDROID__*/
  return 0;
}

void UniformGlue::onGLFWFocus(GLFWwindow* window, int focused) {
  auto self = reinterpret_cast<UniformGlue*>(glfwGetWindowUserPointer(window));
  self->focused = focused;
#ifdef __ANDROID__
  if (!focused) {
    // On Android, surface *must* *not* be used after this function returns.
    self->app.cpool.vk.dev.destroySurface();
  }
  self->prevEntered = focused;
#else  /*__ANDROID__*/
  // Apply focus state to enter/leave state and update app. GLFW does not.
  int entered = self->prevEntered;
  if (!focused) {
    entered = 0;
  } else {
    // Non-Android GLFW, even with multitouch, can still get cursor pos.
    GLFWinputEvent ie;
    glfwGetCursorPos(window, &ie.x, &ie.y);
    applyPreTransform(self->app, ie.x, ie.y);
    auto& imageExtent = self->app.cpool.vk.dev.swapChainInfo.imageExtent;
    entered = (ie.x >= 0 && ie.y >= 0 && ie.x < imageExtent.width &&
               ie.y < imageExtent.height);
  }
  onGLFWcursorEnter(window, entered);
#endif /*__ANDROID__*/
}

#if defined(GLFW_HAS_MULTITOUCH) && !defined(VOLCANO_TEST_NO_MULTITOUCH)
void UniformGlue::onGLFWmultitouch(GLFWwindow* window, GLFWinputEvent* events,
                                   int eventCount, int mods) {
  auto self = reinterpret_cast<UniformGlue*>(glfwGetWindowUserPointer(window));
  self->processInputEvents(events, eventCount, mods);
  self->prevMods = mods;
}

#else  /*GLFW_HAS_MULTITOUCH*/

void UniformGlue::onGLFWcursorPos(GLFWwindow* window, double x, double y) {
  auto self = reinterpret_cast<UniformGlue*>(glfwGetWindowUserPointer(window));
  GLFWinputEvent ie;
  ie.inputDevice = GLFW_INPUT_FIXED;
  ie.num = 0;
  ie.hover = 0;
  ie.x = x;
  ie.y = y;
  ie.buttons = self->prevMouseButtons;
  ie.xoffset = 0;
  ie.yoffset = 0;
  ie.action = GLFW_CURSORPOS;
  ie.actionButton = 0;

  self->processInputEvents(&ie, 1, self->prevMods);
}

void UniformGlue::onGLFWmouseButtons(GLFWwindow* window, int button, int action,
                                     int mods) {
  auto self = reinterpret_cast<UniformGlue*>(glfwGetWindowUserPointer(window));
  if (action == GLFW_PRESS) {
    self->prevMouseButtons |= 1 << button;
  } else {
    self->prevMouseButtons &= ~(1 << button);
  }
  GLFWinputEvent ie;
  ie.inputDevice = GLFW_INPUT_FIXED;
  ie.num = 0;
  ie.hover = 0;
  glfwGetCursorPos(window, &ie.x, &ie.y);
  ie.buttons = self->prevMouseButtons;
  ie.xoffset = 0;
  ie.yoffset = 0;
  ie.action = action;
  ie.actionButton = 1 << button;

  self->processInputEvents(&ie, 1, mods);
  self->prevMods = mods;
}

void UniformGlue::onGLFWscroll(GLFWwindow* window, double scroll_x,
                               double scroll_y) {
  auto self = reinterpret_cast<UniformGlue*>(glfwGetWindowUserPointer(window));
  GLFWinputEvent ie;
  ie.inputDevice = GLFW_INPUT_FIXED;
  ie.num = 0;
  ie.hover = 0;
  glfwGetCursorPos(window, &ie.x, &ie.y);
  ie.buttons = self->prevMouseButtons;
  ie.xoffset = scroll_x;
  ie.yoffset = scroll_y;
  ie.action = GLFW_SCROLL;
  ie.actionButton = 0;

  self->processInputEvents(&ie, 1, self->prevMods);
}
#endif /*GLFW_HAS_MULTITOUCH*/

void UniformGlue::onGLFWcursorEnter(GLFWwindow* window, int entered) {
  auto self = reinterpret_cast<UniformGlue*>(glfwGetWindowUserPointer(window));
  if (self->isImguiAvailable() && !entered) {
    auto& io = ImGui::GetIO();
    for (size_t i = 0; i < sizeof(io.MouseDown) / sizeof(io.MouseDown[0]);
         i++) {
      io.MouseDown[i] = false;
    }
  }

  for (auto& p : self->inputEventListeners) {
    p.first(p.second, self->prevInput.data(), self->prevInput.size(),
            self->prevMods, entered);
  }
  self->prevEntered = entered;
}

void UniformGlue::onGLFWcontentScale(GLFWwindow* window, float x, float y) {
  auto self = reinterpret_cast<UniformGlue*>(glfwGetWindowUserPointer(window));
#if defined(__linux__) && !defined(__ANDROID__)
  x = 1.f;  // Not all monitors report correctly on Desktop Linux.
  y = 1.f;
#endif
  self->scaleX = x;
  self->scaleY = y;
  if (self->isImguiAvailable()) {
    auto& io = ImGui::GetIO();
    io.DisplayFramebufferScale = ImVec2(x, y);
  }
  self->needRebuild = true;
}

void UniformGlue::onGLFWkey(GLFWwindow* window, int k, int scancode, int action,
                            int mods) {
  auto self = reinterpret_cast<UniformGlue*>(glfwGetWindowUserPointer(window));
  if (self->isImguiAvailable()) {
    auto& io = ImGui::GetIO();
    if (k >= 0 && size_t(k) < sizeof(io.KeysDown) / sizeof(io.KeysDown[0])) {
#ifdef __ANDROID__
      // Android may send GLFW_RELEASE immediately after GLFW_PRESS. ImGui will
      // not even see the GLFW_PRESS if GLFW_RELEASE clears KeysDown[k] now.
      if (action == GLFW_RELEASE) {
        auto i = self->keyTooFast.find(k);
        if (i != self->keyTooFast.end()) {
          i->second++;  // Remember GLFW_RELEASE event for later.
        }
      } else {
        // Set KeysDown[k] for GLFW_PRESS and GLFW_REPEAT events immediately.
        self->keyTooFast.insert(std::make_pair(k, 0));
        io.KeysDown[k] = 1;
      }
#else  /*__ANDROID__*/
      io.KeysDown[k] = action == GLFW_PRESS;
#endif /*__ANDROID__*/
    }
    io.KeyShift = !!(mods & GLFW_MOD_SHIFT);
    io.KeyCtrl = !!(mods & GLFW_MOD_CONTROL);
    io.KeyAlt = !!(mods & GLFW_MOD_ALT);
    io.KeySuper = !!(mods & GLFW_MOD_SUPER);

    if (io.WantCaptureKeyboard &&  // Swallow keypress *only* if it is mapped.
        io.KeyCtrl) {  // Swallow *only* if Ctrl is held (Ctrl-A, Ctrl-C, etc.)
      for (size_t i = 0; i < ImGuiKey_COUNT; i++) {
        if (io.KeyMap[i] == k) {
          return;
        }
      }
    }
    if (io.WantCaptureKeyboard && ImGui::IsAnyItemActive()) {
      return;  // If an actual widget is consuming it, not just navigation.
    }
  }
  for (auto& p : self->keyEventListeners) {
    p.first(p.second, k, scancode, action, mods);
  }
}

void UniformGlue::onGLFWchar(GLFWwindow* window, unsigned utf32ch) {
  auto self = reinterpret_cast<UniformGlue*>(glfwGetWindowUserPointer(window));
  // Convert utf32 to utf8.
  char utf8[16];
  if (!utf8_encode(utf8, sizeof(utf8), utf32ch)) {
    logE("onGLFWchar: utf8_encode of %x failed\n", utf32ch);
    return;
  }

  if (self->isImguiAvailable()) {
    auto& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) {
      io.AddInputCharactersUTF8(utf8);
      return;
    }
  }
  for (auto& p : self->charEventListeners) {
    p.first(p.second, utf8);
  }
}

std::set<monitorWithName> monitors;

// monitors is a global because GLFW callback lacks any user data pointer.
void onGLFWmonitorChange(GLFWmonitor* /*monitor*/, int /*event*/) {
  int count;
  GLFWmonitor** data = glfwGetMonitors(&count);
  if (!data) {
    logF("glfwGetMonitors failed\n");
    return;
  }
  monitors.clear();
  for (int i = 0; i < count; i++) {
    monitors.insert(monitorWithName(data[i]));
  }
}
