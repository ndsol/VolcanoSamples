/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * RetroCore and RetroApp classes.
 */

#include <src/science/science-glfw.h>

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#define RETROSAVEPROTO_MAGIC "VOLC"
#define RETROSAVEPROTO_ALWAYS_FLAGS (1u << 10)
struct RetroSaveProto {
  // magic must match RETROSAVEPROTO_MAGIC
  char magic[4];
  // saveDataLen is the size of the saveData array.
  uint32_t saveDataLen;
  // flags:
  // 1 << 4: The system that wrote this was little endian.
  // 1 << 5: RETRO_SERIALIZATION_QUIRK_ENDIAN_DEPENDENT
  // 1 << 6: (reserved for RETRO_SERIALIZATION_QUIRK_PLATFORM_DEPENDENT)
  // 1 << 10: must be set to 1 for a valid save.
  uint64_t flags;

  // gameName: name of game file / rom file. If not an exact match, the app
  // does not load the save. This is a very imperfect way to do this.
  char gameName[256];
  // gameType: extension used to match game file / rom file with a core.
  char gameType[4];
  // saveData: core state obtained from retro_serialize().
  char saveData[];
};

inline size_t protoSize(size_t saveDataLen) {
  return sizeof(RetroSaveProto) + saveDataLen;
}

static constexpr size_t maxSaveDataLen = 256llu * 1024llu * 1024llu;

// RetroUIinterface defines the methods a UI must implement.
// This is an abstract base class. (No members and only pure-virtual methods.)
struct RetroUIinterface {
  // videoRefresh is called if the core does not setup a different video output
  // method. This is the default method: data contains image pixels.
  // Vulkan formats include an alpha channel, but data's byte in the alpha
  // channel *must* be overwritten with a constant.
  virtual void videoRefresh(const void* data, unsigned width, unsigned height,
                            size_t pitch, VkFormat format) = 0;
  // audioBatch is called with some audio data. The length of the data array is
  // 2 * frames like { left0, right0, left1, right1, ... leftN, rightN }.
  // audioBatch should return the number of frames it was able to buffer,
  // some number between 1 and 'frames'.
  virtual size_t audioBatch(const int16_t* data, size_t frames, int rate) = 0;
  // showMessage is used by some games.
  virtual void showMessage(const char* msg, unsigned frames) = 0;
  // setPerfLevel can happen any time during a game. The values are arbitrary,
  // but higher means a more demanding emulation task.
  virtual void setPerfLevel(unsigned lvl) = 0;
};

struct InputDesc {
  InputDesc(unsigned index) : index(index) {}

  int checkValid() {
    if (desc.empty()) {
      logE("desc[%u] invalid: empty description\n", index);
      return 1;
    }
    return 0;
  }

  const unsigned index;
  std::string desc;

  // state should be updated before nextFrame() is called.
  int16_t state{0};
};

struct InputAxis {
  InputAxis(unsigned id) : id(id) {}

  int checkValid() {
    for (auto i = desc.begin(); i != desc.end();) {
      if (i->second.checkValid()) {
        logW("axis %u: invalid desc[%u] (removed)\n", id, i->first);
        i = desc.erase(i);
      } else {
        i++;
      }
    }
    if (desc.empty()) {
      logE("axis %u invalid: empty desc set\n", id);
      return 1;
    }
    return 0;
  }

  const unsigned id;
  std::map<unsigned, InputDesc> desc;
};

// InputPortChoice is one value the core supplied with SET_CONTROLLER_INFO.
struct InputPortChoice {
  InputPortChoice(unsigned device) : device(device) {}
  const unsigned device;
  std::string desc;
};

// InputPort represents a real "port" or plug to plug in a controller / joypad.
// Each player would usually get one port, and there are usually no more
// than four ports - meaning a 4-player game is possible.
//
// "By default, RETRO_DEVICE_JOYPAD is assumed" for cores that emulate a game
// system. Other cores may have a keyboard, mouse, etc.
//
// 'device'  is the default or initial device the core announced in
// SET_INPUT_DESCRIPTORS. That gives an idea of what kind of port. A joypad
// port may support other devices though like a lightgun or mouse -
// SET_CONTROLLER_INFO lists the other choices known to plug into this port.
// Call retro_set_controller_port_device() to tell the core a different device
// was plugged into this port.
//
// Cores fall into two broad groups:
// 1. Cores with a keyboard and mouse. For example, dosbox.
// 2. Cores with 1-4 joypads but no keyboard. For example, Atari or NES.
//
// Whatever is plugged in then has many axis it supports. (Keys on a keyboard,
// buttons and sticks on a joypad, mouse axes, etc.)
struct InputPort {
  InputPort(unsigned port, unsigned device) : port(port), device(device) {}

  int checkValid() {
    for (auto i = axis.begin(); i != axis.end();) {
      if (i->second.checkValid()) {
        logW("port %u: invalid axis %u/%u (removed)\n", port, port, i->first);
        i = axis.erase(i);
      } else {
        i++;
      }
    }
    if (axis.empty()) {
      logE("port %u invalid: empty axis set\n", port);
      return 1;
    }
    return 0;
  }

  const unsigned port, device;
  std::map<unsigned, InputAxis> axis;
  std::vector<InputPortChoice> choice;

  // hotplugDevice defaults to RETRO_DEVICE_NONE (0). If the core asks for a
  // device that was unexpected, hotplugDevice becomes that device.
  unsigned hotplugDevice{0};
};

// SubsystemRomMem describes the file loaded into a memory bank.
struct SubsystemRomMem {
  SubsystemRomMem(unsigned type) : type(type) {}
  const unsigned type;
  std::string support;  // Should only have one extension supported.
};

// SubsystemRom describes a rom file loaded to boot a subsystem.
struct SubsystemRom {
  std::string desc;                  // Long human-readable description.
  std::vector<std::string> support;  // File extensions supported.
  bool need_fullpath{false};
  bool block_extract{false};
  bool required{false};
  std::vector<SubsystemRomMem> mem;
};

// Subsystem describes a secondary game system included in the main core,
// but that is not normally active, and may require more rom files to run.
//
// TODO: Subsystem is not implemented yet.
struct Subsystem {
  Subsystem(unsigned id) : id(id) {}
  const unsigned id;
  std::string desc;   // Long human-readable description.
  std::string ident;  // Short identifier e.g. for a CLI.
  std::vector<SubsystemRom> rom;
};

struct retro_controller_info;
struct retro_input_descriptor;
struct retro_system_info;
struct retro_variable;
struct core_private;
struct RetroWeb;

struct RetroVar {
  RetroVar(const struct retro_variable* def);
  const std::string key;
  std::string desc;
  std::vector<std::string> choice;
  size_t current{0};
  bool changed{false};

  const char* get() const {
    if (current >= choice.size()) {
      return choice.at(0).c_str();
    }
    return choice.at(current).c_str();
  }

  void setCurrent(size_t i) {
    current = i;
    changed = true;
  }

  int setCurrent(const std::string& val) {
    for (size_t i = 0; i < choice.size(); i++) {
      if (choice.at(i) == val) {
        setCurrent(i);
        return 0;
      }
    }
    return 1;
  }
};

// RETROCORE_KEY_MAX is the highest keyboard keycode supported.
#define RETROCORE_KEY_MAX (324)

typedef struct RetroCore {
  RetroCore(RetroWeb& parent, const std::string& corepath);
  ~RetroCore();

  // getCorepath returns the corepath of this core.
  const std::string& getCorepath() const { return corepath; }

  // getName returns the library_name in the core.
  const std::string& getName() const { return name; }

  // ctorError loads the core from corepath.
  int ctorError();

  // getSupport may return an empty vector if called before ctorError().
  // Otherwise returns one string for each type of rom file supported.
  // NOTE: A self-contained core will return empty but getNoROM() == true.
  const std::vector<std::string>& getSupport() const { return support; }

  bool getNoRom() const { return noRom; }

  int setRomPath(const std::string& pathname, off_t filesize);

  int launchRom();

  // nextFrame runs the core. It is not an error to pause calling nextFrame,
  // that is how pause is implemented.
  int nextFrame(RetroUIinterface& ui);

  // updateJoypad writes joypad state to the core. It is not an error if the
  // core does not support a joypad or a given id. (Only a warning is printed.)
  int updateJoypad(unsigned port, unsigned id, int16_t state);

  // save writes binary data to dst which can later be used to load.
  // returns 0=good save, 1=failure
  // NOTE: isLastSaveTooEarly() returns true after a save fails if the core
  //       has not run long enough yet (certain cores need time to boot up).
  int save(std::vector<char>& dst);

  // load reads binary data from src and loads it into the core.
  int load(const std::vector<char>& src);

  bool isLastSaveTooEarly() const { return wasLastSaveTooEarly; }

  int setVar(const std::string& key, const std::string& val) {
    auto i = vars.find(key);
    if (i == vars.end()) {
      logW("setVar(%s, %s): not found\n", key.c_str(), val.c_str());
      return 1;
    }
    return i->second.setCurrent(val);
  }

  int setVarToNext(const std::string& key) {
    auto i = vars.find(key);
    if (i == vars.end()) {
      logW("setVarToNext(%s): not found\n", key.c_str());
      return 1;
    }
    size_t n = i->second.current + 1;
    if (n >= i->second.choice.size()) {
      n = 0;
    }
    i->second.setCurrent(n);
    return 0;
  }

  std::map<unsigned, InputPort> ports;

  // keyState provides the complete state of known keys. Your app should update
  // keyState each frame before calling nextFrame().
  std::array<int, RETROCORE_KEY_MAX> keyState;

  // keyToJoypad is used if mapKeysToJoypad is true. It turns each
  // RETROCORE_KEY_xxx value into a RETRO_DEVICE_ID_JOYPAD_xxx value (to
  // simulate a joypad with a keyboard). Your app should set up the key mapping
  // before setting mapKeysToJoypad to true.
  std::vector<std::pair<int, int>> keyToJoypad;

  // mouseX, mouseY give the mouse position scaled to [-0x7fff, 0x7fff].
  // mouseEnter and mouseButton should be either 0 or 1. Your app should update
  // the mouse position before calling nextFrame().
  int16_t mouseX, mouseY, mouseEnter, mouseButton;

  // mapKeysToJoypad when true tells inputUpdate() to ignore any InputPort
  // where device == RETRO_DEVICE_KEYBOARD and update joypad(s) instead using
  // keyToJoypad.
  bool mapKeysToJoypad{true};

  bool getHasKeyboard() const { return hasKeyboard; }

  // subsys is collected from the core but not implemented yet.
  std::vector<Subsystem> subsys;

 protected:
  friend struct core_private;
  bool environment(unsigned cmd, void* data);

  // setPixelFormat receives an enum retro_pixel_format. The type here is 'int'
  // simply to not need a dependency on libretro.h.
  bool setPixelFormat(int fmt);

  // setInputDescriptors receives an array of retro_input_descriptor.
  bool setInputDescriptors(const struct retro_input_descriptor* desc);

  // addControllerArray receives an array of retro_controller_info.
  void addControllerArray(const struct retro_controller_info* con);

  // mapInput builds the mapping from system input to core input.
  int mapInput();

  // inputUpdate reads keyState and updates ports. The core should call
  // input_poll(), which calls inputUpdate().
  int inputUpdate();

  // loadRom loads the rom into memory in the core.
  int loadRom();

  // resetCore sends a "system reset" signal to the core.
  int resetCore();

  int16_t getHotplugState(InputPort& p, unsigned index, unsigned id);

  const std::string& getCachePath() const;
  const std::string& getSystemDir() const;
  const std::string& getSavePath() const;

  RetroWeb& parent;
  size_t frameCount{0};
  // corepath is used to dlopen() the core.
  const std::string corepath;
  // dlh contains the result of calling dlopen().
  void* dlh{nullptr};
  // inited is set to true if the core's retro_init() was called.
  bool inited{false};
  // gameLoaded is set to true if the core's retro_load_game() was called.
  bool gameLoaded{false};
  // hasKeyboard indicates there is an InputPort with RETRO_DEVICE_KEYBOARD.
  bool hasKeyboard{false};
  // needChange is if the core was just reset. Var changes are not known yet.
  bool needChange{false};
  // system_info is filled in ctorError.
  std::shared_ptr<struct retro_system_info> systemInfo;
  // name is used to not re-add this core again in RetroWeb.
  std::string name;
  // support contains one string for each type of rom file this core supports.
  // e.g. "nes" if it can load "*.nes" files. "a26" if it can load "*.a26".
  // Each string will be all-lowercase and omits the dot before the extension.
  std::vector<std::string> support;
  // core can set variables, each maps to a string value.
  // NOTE: Any update to vars must also set varsChanged = true;
  std::map<std::string, RetroVar> vars;

  uint64_t serializeQuirks{0};
  bool wasLastSaveTooEarly{false};
  void setupSerializer(uint64_t& quirks);

  // pixelFmt is a translation of enum retro_pixel_format into VkFormat.
  VkFormat pixelFmt;

  // noRom is true if this core does not need or use a rom file.
  bool noRom{false};
  // romPath is the full path to the rom file.
  std::string romPath;
  off_t romSize;
  // romData is populated after launchRom (if need_fullpath).
  std::vector<char> romData;

  int16_t emulatedMouseX{0};
  int16_t emulatedMouseY{0};

  std::shared_ptr<struct core_private> priv;
} RetroCore;

typedef struct RetroApp {
 public:
  RetroApp(RetroCore& core, const std::string& fileType)
      : core(core), fileType(fileType) {
    if (core.getNoRom()) {
      name = core.getName();
    }
  }
  ~RetroApp();
  // getName returns the name of the app. If core.getNoRom() then this is
  // the same as core.getName(). Otherwise load() will set the name.
  const std::string& getName() const { return name; }
  const std::string& getType() const { return fileType; }
  bool isOpen() const { return running; }
  RetroCore& core;

  // load sets up this app to run the specified rom image.
  // NOTE: To not chew up ram, the rom image is not kept in memory, but will be
  // re-loaded during open().
  int load(const std::string& pathname, off_t filesize, const char* filename);

  // open starts the app. Then call nextFrame() to run the app.
  int open();
  // close stops the app.
  int close();

  int nextFrame(RetroUIinterface& ui);

  void setPerfLevel(unsigned lvl) {
    if (perfLevelIsValid && perfLevel >= lvl) {
      return;
    }
    perfLevelIsValid = true;
    perfLevel = lvl;
  }

 protected:
  std::string name;
  std::string romPath;
  std::string fileType;
  off_t romSize;
  bool running{false};
  bool perfLevelIsValid{false};
  unsigned perfLevel;
} RetroApp;
