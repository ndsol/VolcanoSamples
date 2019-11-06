/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * RetroCore input code.
 */

#include "include/libretro.h"
#include "retrocore.h"

bool RetroCore::setInputDescriptors(const struct retro_input_descriptor* i) {
  for (; i->description; i++) {
    auto port = ports.find(i->port);
    if (port == ports.end()) {
      auto p = ports.emplace(i->port, InputPort{i->port, i->device});
      if (!p.second) {
        logE("setInputDescriptors: BUG: emplace(%u, %u) already present?\n",
             i->port, i->device);
      }
      port = p.first;
    }

    auto& portAxis = port->second.axis;
    auto axis = portAxis.find(i->id);
    if (axis == portAxis.end()) {
      auto p = portAxis.emplace(i->id, i->id);
      if (!p.second) {
        logE("setInputDescriptors: BUG: %u/%u axis.emplace(%u) already here?\n",
             i->port, i->device, i->id);
      }
      axis = p.first;
    }

    auto& axisDesc = axis->second.desc;
    auto desc = axisDesc.find(i->index);
    if (desc == axisDesc.end()) {
      auto p = axisDesc.emplace(i->index, i->index);
      if (!p.second) {
        logE("setInputDescriptors: BUG: %u/%u/%u desc.emplace(%u) already?\n",
             i->port, i->device, i->id, i->index);
      }
      desc = p.first;
    }
    desc->second.desc = i->description;
    if (0)
      logI("%u/%u/%u/%u %s\n", i->port, i->device, i->id, i->index,
           i->description);
  }
  return true;
}

void RetroCore::addControllerArray(const struct retro_controller_info* con) {
  // con is terminated with a struct filled with 0's.
  for (unsigned i = 0; con->num_types; con++, i++) {
    auto port = ports.find(i);
    if (port == ports.end()) {
      // It is known con->num_types > 0. Assume all types subclass the same
      // baseDevice:
      unsigned baseDevice = con->types[0].id & RETRO_DEVICE_MASK;
      auto p = ports.emplace(i, InputPort{i, baseDevice});
      if (!p.second) {
        logE("addControllerArray: BUG: emplace(%u, %u) already present?\n", i,
             baseDevice);
      }
      port = p.first;
    }
    std::string allDesc;
    for (size_t j = 0; j < con->num_types; j++) {
      auto& o = con->types[j];
      allDesc += " \"";
      allDesc += o.desc;
      allDesc += '"';
      port->second.choice.emplace_back(o.id);
      auto& c = port->second.choice.back();
      c.desc = o.desc;
    }
    if (0) logI("core port=%u can be: %s\n", i, allDesc.c_str());
  }
}

int RetroCore::mapInput() {
  char description[256];
  hasKeyboard = false;
  if (RETROK_LAST != RETROCORE_KEY_MAX) {
    logE("mapInput: RETROK_LAST=%u RETROCORE_KEY_MAX=%u - should be equal!\n",
         (unsigned)RETROK_LAST, (unsigned)RETROCORE_KEY_MAX);
  }
  for (auto i = ports.begin(); i != ports.end();) {
    auto& port = i->second;
    bool valid = true;
    switch (port.device) {
      case RETRO_DEVICE_NONE:
        logW("mapInput: port %u null device %u ignored\n", i->first,
             port.device);
        break;
      case RETRO_DEVICE_JOYPAD:
        // Add any missing buttons. Cores do not always list them all.
        for (unsigned id = 0; id <= RETRO_DEVICE_ID_JOYPAD_R3; id++) {
          auto axis = port.axis.find(id);
          if (axis != port.axis.end()) {
            continue;
          }
          auto p = port.axis.emplace(id, id);
          if (!p.second) {
            logE("mapInput: BUG: %u/%u axis.emplace(%u) already here?\n",
                 i->first, port.device, id);
          }
          axis = p.first;

          auto& axisDesc = axis->second.desc;
          unsigned index = 0;
          auto desc = axisDesc.find(index);
          if (desc == axisDesc.end()) {
            auto q = axisDesc.emplace(index, index);
            if (!q.second) {
              logE("mapInput: BUG: %u/%u/%u desc.emplace(%u) already?\n",
                   i->first, port.device, id, index);
            }
            desc = q.first;
          }
          snprintf(description, sizeof(description), "joypad_%u", id);
          desc->second.desc = description;
          if (0)
            logI("%u/%u/%u/%u (auto)%s\n", i->first, port.device, id, index,
                 description);
        }
        break;
      case RETRO_DEVICE_MOUSE:
        break;
      case RETRO_DEVICE_KEYBOARD:
        if (hasKeyboard) {
          logW("mapInput: dev %u/%u - multiple keyboards not supported\n",
               i->first, port.device);
          break;
        }
        hasKeyboard = true;
        // Add any missing keys. Cores do not always list them all.
        for (unsigned id = 0; id <= RETROK_LAST; id++) {
          auto key = port.axis.find(id);
          if (key != port.axis.end()) {
            continue;
          }
          auto p = port.axis.emplace(id, id);
          if (!p.second) {
            logE("mapInput: BUG: %u/%u key.emplace(%u) already here?\n",
                 i->first, port.device, id);
          }
          key = p.first;

          auto& keyDesc = key->second.desc;
          unsigned index = 0;
          auto desc = keyDesc.find(index);
          if (desc == keyDesc.end()) {
            auto q = keyDesc.emplace(index, index);
            if (!q.second) {
              logE("mapInput: BUG: %u/%u/%u desc.emplace(%u) already?\n",
                   i->first, port.device, id, index);
            }
            desc = q.first;
          }
          snprintf(description, sizeof(description), "key_%u", id);
          desc->second.desc = description;
          if (0)
            logI("%u/%u/%u/%u (auto)%s\n", i->first, port.device, id, index,
                 description);
        }
        break;
      case RETRO_DEVICE_LIGHTGUN:
        break;
      case RETRO_DEVICE_ANALOG:
        logW("mapInput: dev %u/%u not implemented\n", i->first, port.device);
        break;
      case RETRO_DEVICE_POINTER:
        break;
      default:
        logW("mapInput: port %u unknown device %u (removed)\n", i->first,
             port.device);
        valid = false;
        break;
    }
    if (!valid || port.checkValid()) {
      logW("mapInput: invalid port %u (removed)\n", i->first);
      i = ports.erase(i);
    } else {
      i++;
    }
  }
  return 0;
}

int RetroCore::inputUpdate() {
  if (!mapKeysToJoypad) {
    for (auto i = ports.begin(); i != ports.end(); i++) {
      auto& port = i->second;
      if (port.device != RETRO_DEVICE_KEYBOARD) {
        continue;
      }
      for (auto j = port.axis.begin(); j != port.axis.end(); j++) {
        auto& key = j->second;
        if (key.id >= keyState.size()) {
          logW("port %u key %u invalid\n", port.port, key.id);
          continue;
        }
        for (auto k = key.desc.begin(); k != key.desc.end(); k++) {
          k->second.state = keyState.at(key.id);
        }
      }
    }
    return 0;
  }

  // Map keyboard-to-gamepad and write to all RETRO_DEVICE_JOYPAD.
  for (auto i = ports.begin(); i != ports.end(); i++) {
    auto& port = i->second;
    if (port.device != RETRO_DEVICE_JOYPAD) {
      continue;
    }
    for (auto j = keyToJoypad.begin(); j != keyToJoypad.end(); j++) {
      if (updateJoypad(port.port, j->second, keyState.at(j->first))) {
        logE("updateJoypad(port %u, id %u, %d) failed\n", port.port, j->second,
             keyState.at(j->first));
        return 1;
      }
    }
    return 0;
  }
  logW("inputUpdate: mapKeysToJoypad == true but no joypad found\n");
  return 0;
}

int RetroCore::updateJoypad(unsigned port, unsigned id, int16_t state) {
  auto i = ports.find(port);
  if (i == ports.end()) {
    logW("updateJoypad(port %u, id %u, %d): no port %u\n", port, id, state,
         port);
    return 0;
  }
  auto a = i->second.axis.find(id);
  if (a == i->second.axis.end()) {
    logW("updateJoypad(port %u, id %u, %d): no axis %u\n", port, id, state, id);
    return 0;
  }
  auto& axis = a->second;
  for (auto k = axis.desc.begin(); k != axis.desc.end(); k++) {
    k->second.state = state;
  }
  return 0;
}

int16_t RetroCore::getHotplugState(InputPort& p, unsigned index, unsigned id) {
  switch (p.hotplugDevice) {
    case RETRO_DEVICE_MOUSE:
      switch (id) {
        case RETRO_DEVICE_ID_MOUSE_X: {
          int16_t d = mouseX - emulatedMouseX;
          emulatedMouseX = mouseX;
          return d;
        }
        case RETRO_DEVICE_ID_MOUSE_Y: {
          int16_t d = mouseY - emulatedMouseY;
          emulatedMouseY = mouseY;
          return d;
        }
        case RETRO_DEVICE_ID_MOUSE_LEFT:
          return mouseButton;
        default:
          return 0;
      }
    case RETRO_DEVICE_LIGHTGUN:
      switch (id) {
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X:
          return mouseEnter ? -0x8000 : mouseX;
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y:
          return mouseEnter ? -0x8000 : mouseY;
        case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN:
          return !mouseEnter;
        case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER:
          return mouseButton;
        default:
          return 0;
      }
    case RETRO_DEVICE_POINTER:
      if (index != 0) {
        // Request for second finger, or higher.
        return 0;
      }
      switch (id) {
        case RETRO_DEVICE_ID_POINTER_X:
          return mouseX;
        case RETRO_DEVICE_ID_POINTER_Y:
          return mouseY;
        case RETRO_DEVICE_ID_POINTER_PRESSED:
          return mouseButton;
        case RETRO_DEVICE_ID_POINTER_COUNT:
          return mouseEnter;
        default:
          return 0;
      }
    default:
      return 0;
  }
}
