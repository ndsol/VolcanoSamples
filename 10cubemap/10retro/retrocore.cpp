/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * RetroCore implementation.
 */

#include "retrocore.h"

#ifdef WIN32
#include <windows.h>
#include <winsock.h>  // for htonl()

static char winLoadLibraryErrBuf[512];
#else                   /*WIN32*/
#include <arpa/inet.h>  // for htonl()
#include <dlfcn.h>
#endif /*WIN32*/
#include <ctype.h>
#include <errno.h>
#include <math.h>

#include "include/libretro.h"

typedef RETRO_CALLCONV void (*fn_retro_init)(void);
typedef RETRO_CALLCONV void (*fn_retro_deinit)(void);
typedef RETRO_CALLCONV unsigned (*fn_retro_api_version)(void);
typedef RETRO_CALLCONV void (*fn_retro_get_system_info)(
    struct retro_system_info* info);
typedef RETRO_CALLCONV void (*fn_retro_get_system_av_info)(
    struct retro_system_av_info* info);
typedef RETRO_CALLCONV void (*fn_retro_set_controller_port_device)(
    unsigned port, unsigned device);
typedef RETRO_CALLCONV void (*fn_retro_reset)(void);
typedef RETRO_CALLCONV void (*fn_retro_run)(void);
typedef RETRO_CALLCONV void (*fn_retro_set_environment)(
    retro_environment_t env);
typedef RETRO_CALLCONV void (*fn_retro_set_video_refresh)(
    retro_video_refresh_t r);
typedef RETRO_CALLCONV void (*fn_retro_set_audio_sample)(
    retro_audio_sample_t as);
typedef RETRO_CALLCONV void (*fn_retro_set_audio_sample_batch)(
    retro_audio_sample_batch_t asb);
typedef RETRO_CALLCONV void (*fn_retro_set_input_poll)(retro_input_poll_t i);
typedef RETRO_CALLCONV void (*fn_retro_set_input_state)(retro_input_state_t i);
typedef RETRO_CALLCONV bool (*fn_retro_load_game)(
    const struct retro_game_info* game);
typedef RETRO_CALLCONV void (*fn_retro_unload_game)(void);
typedef RETRO_CALLCONV size_t (*fn_retro_serialize_size)(void);
typedef RETRO_CALLCONV size_t (*fn_retro_serialize)(void* data, size_t size);
typedef RETRO_CALLCONV size_t (*fn_retro_unserialize)(const void* data,
                                                      size_t size);

static struct core_private* oneActiveCore = nullptr;
int setActiveCore(struct core_private* p) {
  if (oneActiveCore) {
    logE("oneActiveCore only (%p), cannot set %p\n", oneActiveCore, p);
    return 1;
  }
  oneActiveCore = p;
  return 0;
}

void clearActiveCore() { oneActiveCore = nullptr; }

struct core_private {
  core_private() {
    memset(&frame_time, 0, sizeof(frame_time));
    memset(&av_info, 0, sizeof(av_info));
  }
#define ADD_CB(name) \
  fn_##name name { nullptr }
  ADD_CB(retro_init);
  ADD_CB(retro_deinit);
  ADD_CB(retro_api_version);
  ADD_CB(retro_get_system_info);
  ADD_CB(retro_get_system_av_info);
  ADD_CB(retro_set_controller_port_device);
  ADD_CB(retro_reset);
  ADD_CB(retro_run);
  ADD_CB(retro_set_environment);
  ADD_CB(retro_set_video_refresh);
  ADD_CB(retro_set_audio_sample);
  ADD_CB(retro_set_audio_sample_batch);
  ADD_CB(retro_set_input_poll);
  ADD_CB(retro_set_input_state);
  ADD_CB(retro_load_game);
  ADD_CB(retro_unload_game);
  ADD_CB(retro_serialize_size);
  ADD_CB(retro_serialize);
  ADD_CB(retro_unserialize);
#undef ADD_CB
  struct retro_frame_time_callback frame_time;
  retro_get_proc_address_t get_proc_address{nullptr};
  struct retro_system_av_info av_info;
  std::vector<struct retro_memory_descriptor> memmap;
  int errorCount{0};
  static bool noisy() { return oneActiveCore->errorCount > 9; }

  RetroCore* core{nullptr};
  RetroUIinterface* ui{nullptr};
  bool earlyPerfLevel{false};
  unsigned perfLevel{0};
  static bool RETRO_CALLCONV environment(unsigned cmd, void* data) {
    return oneActiveCore->core->environment(cmd, data);
  }
  static void RETRO_CALLCONV video_refresh(const void* data, unsigned width,
                                           unsigned height, size_t pitch) {
    if (!oneActiveCore->ui) {
      if (!noisy()) logE("video_refresh outside of normal context\n");
      oneActiveCore->errorCount++;
      return;
    }
    oneActiveCore->ui->videoRefresh(data, width, height, pitch,
                                    oneActiveCore->core->pixelFmt);
  }
  static void RETRO_CALLCONV audio_sample(int16_t left, int16_t right) {
    if (!oneActiveCore->ui) {
      if (!noisy()) logE("audio_sample outside of normal context\n");
      oneActiveCore->errorCount++;
      return;
    }
    if (!oneActiveCore->av_info.timing.sample_rate) {
      if (!noisy()) logE("audio_sample without sample_rate\n");
      oneActiveCore->errorCount++;
      return;
    }
    int16_t data[2] = {left, right};
    int rate = (int)oneActiveCore->av_info.timing.sample_rate;
    oneActiveCore->ui->audioBatch(data, 1, rate);
  }
  static size_t RETRO_CALLCONV audio_sample_batch(const int16_t* data,
                                                  size_t frames) {
    if (!oneActiveCore->ui) {
      if (!noisy()) logE("audio_sample_batch outside of normal context\n");
      oneActiveCore->errorCount++;
      return frames;
    }
    if (!oneActiveCore->av_info.timing.sample_rate) {
      if (!noisy()) logE("audio_sample_batch without sample_rate\n");
      oneActiveCore->errorCount++;
      return frames;
    }
    int rate = (int)oneActiveCore->av_info.timing.sample_rate;
    return oneActiveCore->ui->audioBatch(data, frames, rate);
  }
  static void RETRO_CALLCONV input_poll() {
    if (!oneActiveCore->ui) {
      if (!noisy()) logE("input_poll outside of normal context\n");
      oneActiveCore->errorCount++;
      return;
    }
    oneActiveCore->core->inputUpdate();
  }
  static int16_t RETRO_CALLCONV input_state(unsigned port, unsigned device,
                                            unsigned index, unsigned id) {
    if (!oneActiveCore->ui) {
      if (!noisy()) logE("input_state outside of normal context\n");
      oneActiveCore->errorCount++;
      return 0;
    }
    auto& ports = oneActiveCore->core->ports;
    auto p = ports.find(port);
    if (p == ports.end()) {
      if (!noisy()) {
        logE("input_state(%u/%u/%u/%u): port not found\n", port, device, index,
             id);
      }
      oneActiveCore->errorCount++;
      return 0;
    }
    if (device != p->second.device) {
      if (device == RETRO_DEVICE_JOYPAD || device == RETRO_DEVICE_NONE) {
        // If this port does not have this device type, fix the core so it
        // gets it right from the start.
        return 0;
      }
      // Core thinks this is a different device.
      p->second.hotplugDevice = device;
      return oneActiveCore->core->getHotplugState(p->second, index, id);
    }
    auto a = p->second.axis.find(id);
    if (a == p->second.axis.end()) {
      if (!noisy()) {
        logE("input_state(%u/%u/%u/%u): axis not found\n", port, device, index,
             id);
      }
      oneActiveCore->errorCount++;
      return 0;
    }
    auto d = a->second.desc.find(index);
    if (d == a->second.desc.end()) {
      if (!noisy()) {
        logE("input_state(%u/%u/%u/%u): index not found\n", port, device, index,
             id);
      }
      oneActiveCore->errorCount++;
      return 0;
    }
    return d->second.state;
  }
  static void RETRO_CALLCONV log_printf(enum retro_log_level level,
                                        const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char volcanoL;
    switch (level) {
      case RETRO_LOG_DEBUG:
        volcanoL = 'D';
        break;
      case RETRO_LOG_INFO:
        volcanoL = 'I';
        break;
      case RETRO_LOG_WARN:
        volcanoL = 'W';
        break;
      case RETRO_LOG_ERROR:
        volcanoL = 'E';
        break;
      default:
        volcanoL = 'V';
        break;
    }
    logVolcano(volcanoL, fmt, ap);
    va_end(ap);
  }

  void addSubsystem(const struct retro_subsystem_info* sub) {
    if (0) logI("core supports \"%s\"\n", sub->desc);
    core->subsys.emplace_back(sub->id);
    auto& b = core->subsys.back();
    b.desc = sub->desc;
    b.ident = sub->ident;
    auto* subrom = sub->roms;
    for (unsigned i = 0; i < sub->num_roms; i++, subrom++) {
      b.rom.emplace_back();
      auto& r = b.rom.back();
      r.desc = subrom->desc;
      r.need_fullpath = subrom->need_fullpath;
      r.block_extract = subrom->block_extract;
      r.required = subrom->required;
      const char* e = subrom->valid_extensions;
      while (*e) {
        int len = strcspn(e, "|");
        std::string oneExt(e, e + len);
        for (size_t j = 0; j < oneExt.size(); j++) {
          oneExt[j] = tolower(oneExt[j]);
        }
        // Filter out extensions that are too generic.
        if (oneExt != "bin" && oneExt != "bs") {
          r.support.emplace_back(oneExt);
        }
        e += len;
        if (*e) e++;
      }
      auto* submem = subrom->memory;
      for (unsigned j = 0; j < subrom->num_memory; j++, submem++) {
        r.mem.emplace_back(submem->type);
        auto& m = r.mem.back();
        m.support = submem->extension;
      }
    }
  }
};

// loadCore returns 0 on success or non-zero on error.
// *out is written with the result of loading the library.
static int loadCore(const char* path, void** out) {
  void* dlh;
#ifdef _WIN32
  int prevmode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
  int len = 0;
  UINT tryCodePage[] = {CP_UTF8, CP_ACP};
  wchar_t* pathW = NULL;
  for (int i = 0; i < sizeof(tryCodePage) / sizeof(tryCodePage[0]); i++) {
    len = MultiByteToWideChar(tryCodePage[i], 0, path, -1, NULL, 0);
    if (len) {
      pathW = reinterpret_cast<wchar_t*>(calloc(len, sizeof(wchar_t)));
      if (!pathW) {
        logE("loadCore(%s): calloc(%d x %zu) failed\n", path, len,
             sizeof(wchar_t));
        SetErrorMode(prevmode);
        return 1;
      }
      len = MultiByteToWideChar(tryCodePage[i], 0, path, -1, pathW, len);
    }
  }
  if (!pathW) {
    logE("MultiByteToWideChar(%s) failed to get size: %x\n", path,
         GetLastError());
    SetErrorMode(prevmode);
    return 1;
  }
  if (len <= 0) {
    logE("MultiByteToWideChar(%s) failed: %x\n", path, GetLastError());
    SetErrorMode(prevmode);
    return 1;
  }
  dlh = (void*)LoadLibraryW(pathW);
  DWORD e = GetLastError();
  free(pathW);
  SetErrorMode(prevmode);
  if (!dlh) {
    if (!FormatMessage(
            FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM, NULL, e,
            MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT), winLoadLibraryErrBuf,
            sizeof(winLoadLibraryErrBuf) - 1, NULL)) {
      logE("LoadLibrary(%s): %lx\n", path, e);
    } else {
      logE("LoadLibrary(%s): %s\n", path, winLoadLibraryErrBuf);
    }
    return 1;
  }
#else  /*_WIN32*/
  dlh = dlopen(path, RTLD_LAZY);
  if (!dlh) {
    logE("dlopen(%s): %s\n", path, dlerror());
    return 1;
  }
#endif /*_WIN32*/
  *out = dlh;
  return 0;
}

static int unloadCore(void* dlh) {
#ifdef WIN32
  if (!FreeLibrary((HMODULE)dlh)) {
    DWORD e = GetLastError();

    if (!FormatMessage(
            FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM, NULL, e,
            MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT), winLoadLibraryErrBuf,
            sizeof(winLoadLibraryErrBuf) - 1, NULL)) {
      logE("FreeLibrary: %lx\n", e);
    } else {
      logE("FreeLibrary: %s\n", winLoadLibraryErrBuf);
    }
    return 1;
  }
#else  /*WIN32*/
  dlclose(dlh);
#endif /*WIN32*/
  return 0;
}

// coreSym returns NULL on failure.
static void* coreSym(void* dlh, const char* name) {
  void* res;
#ifdef WIN32
  HMODULE mod;
  if (dlh) {
    mod = (HMODULE)dlh;
  } else {
    mod = GetModuleHandle(NULL);
  }
  res = GetProcAddress(mod, name);
  if (!res) {
    DWORD e = GetLastError();

    if (!FormatMessage(
            FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM, NULL, e,
            MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT), winLoadLibraryErrBuf,
            sizeof(winLoadLibraryErrBuf) - 1, NULL)) {
      logE("coreSym(%s): %lx", name, e);
    } else {
      logE("coreSym(%s): %s\n", name, winLoadLibraryErrBuf);
    }
    return NULL;
  }
#else  /*WIN32*/
  if (dlh) {
    res = dlsym(dlh, name);
  } else {
    void* self = dlopen(NULL, RTLD_LAZY);
    if (!self) {
      logE("coreSym(NULL, %s): dlopen(NULL) failed: %s\n", name, dlerror());
      return NULL;
    }
    res = dlsym(self, name);
    dlclose(self);
  }
  if (!res) {
    logE("coreSym(%s): dlsym failed: %s\n", name, dlerror());
    return NULL;
  }
#endif /*WIN32*/
  return res;
}

RetroCore::RetroCore(RetroWeb& parent, const std::string& corepath)
    : parent(parent), corepath(corepath) {
  // libretro defaults to RETRO_PIXEL_FORMAT_0RGB1555.
  setPixelFormat(RETRO_PIXEL_FORMAT_0RGB1555);
};

RetroCore::~RetroCore() {
  if (dlh) {
    if (priv && gameLoaded) {
      setActiveCore(&*priv);
      priv->retro_unload_game();
      clearActiveCore();
    }
    if (priv && inited) {
      setActiveCore(&*priv);
      priv->retro_deinit();
      clearActiveCore();
    }
    if (unloadCore(dlh)) {
      exit(4);
    }
    dlh = nullptr;
  }
}

int RetroCore::ctorError() {
  if (!priv) {
    priv = std::make_shared<struct core_private>();
    priv->core = this;
  }
  if (!dlh) {
    if (loadCore(corepath.c_str(), &dlh)) {
      return 1;
    }
    if (!dlh) {
      logE("loadCore(%s): handle is NULL\n", corepath.c_str());
      return 1;
    }
  }
  if (!priv->retro_get_system_info) {
    void* f;
#define LOAD_FN(name)                                           \
  f = coreSym(dlh, #name);                                      \
  if (!f) {                                                     \
    logE("loadCore(%s): %s failed\n", corepath.c_str(), #name); \
    return 1;                                                   \
  }                                                             \
  memcpy(&priv->name, &f, sizeof(priv->name))
    LOAD_FN(retro_init);
    LOAD_FN(retro_deinit);
    LOAD_FN(retro_api_version);
    LOAD_FN(retro_get_system_info);
    LOAD_FN(retro_get_system_av_info);
    LOAD_FN(retro_set_controller_port_device);
    LOAD_FN(retro_reset);
    LOAD_FN(retro_run);
    LOAD_FN(retro_set_environment);
    LOAD_FN(retro_set_video_refresh);
    LOAD_FN(retro_set_audio_sample);
    LOAD_FN(retro_set_audio_sample_batch);
    LOAD_FN(retro_set_input_poll);
    LOAD_FN(retro_set_input_state);
    LOAD_FN(retro_load_game);
    LOAD_FN(retro_unload_game);
    LOAD_FN(retro_serialize_size);
    LOAD_FN(retro_serialize);
    LOAD_FN(retro_unserialize);
#undef LOAD_FN

    setActiveCore(&*priv);
    systemInfo = std::make_shared<struct retro_system_info>();
    priv->retro_get_system_info(&*systemInfo);
    name = systemInfo->library_name;
    for (size_t i = 0; i < name.size(); i++) {
      name[i] = tolower(name[i]);
    }
    if (priv->retro_api_version() != RETRO_API_VERSION) {
      logW("loaded \"%s\" API v.%u (want %u) - may break\n", name.c_str(),
           priv->retro_api_version(), RETRO_API_VERSION);
    }
    if (systemInfo->valid_extensions) {
      const char* e = systemInfo->valid_extensions;
      bool wantSWC = false;
      bool wantFIG = false;
      bool haveSMC = false;
      while (*e) {
        int len = strcspn(e, "|");
        std::string oneExt(e, e + len);
        for (size_t j = 0; j < oneExt.size(); j++) {
          oneExt[j] = tolower(oneExt[j]);
        }
        // Filter out extensions that are too generic.
        if (oneExt == "swc") {
          wantSWC = true;  // Be careful with weird extension .swc.
        } else if (oneExt == "fig") {
          wantFIG = true;  // Be careful with weird extension .fig.
        } else if (oneExt != "bin" && oneExt != "bs") {
          if (oneExt == "smc") {
            haveSMC = true;
          }
          support.emplace_back(oneExt);
        }
        e += len;
        if (*e) e++;
      }
      // If haveSMC, then ignore the request for SWC.
      if (wantSWC && !haveSMC) {
        logW("core %s supports \"swc\" without \"smc\".\n", getName().c_str());
        support.emplace_back("swc");
      }
      // If haveSMC, then ignore the request for FIG.
      if (wantFIG && !haveSMC) {
        logW("core %s supports \"fig\" without \"smc\".\n", getName().c_str());
        support.emplace_back("fig");
      }
    }
    priv->retro_set_environment(core_private::environment);
    priv->retro_set_video_refresh(core_private::video_refresh);
    priv->retro_set_audio_sample(core_private::audio_sample);
    priv->retro_set_audio_sample_batch(core_private::audio_sample_batch);
    priv->retro_set_input_poll(core_private::input_poll);
    priv->retro_set_input_state(core_private::input_state);
    clearActiveCore();
  }
  needChange = true;
  return 0;
}

int RetroCore::setRomPath(const std::string& pathname, off_t filesize) {
  romPath = pathname;
  romSize = filesize;
  return 0;
}

int RetroCore::loadRom() {
  // Load the file at romPath and pass it to retro_load_game().
  std::shared_ptr<struct retro_game_info> game_info;
  if (!getNoRom()) {
    if (romPath.empty()) {
      logE("launchRom: must call setRomPath first\n");
      return 1;
    }
    if (systemInfo->block_extract) {
      logE("launchRom: block_extract == true, not implemented yet\n");
      return 1;
    }
    game_info = std::make_shared<struct retro_game_info>();
    memset(&*game_info, 0, sizeof(*game_info));
    game_info->path = romPath.c_str();
    if (!systemInfo->need_fullpath) {
      romData.resize(romSize);
      FILE* f = fopen(game_info->path, "rb");
      if (!f) {
        logE("launchRom: failed to open %s: %d %s\n", game_info->path, errno,
             strerror(errno));
        return 1;
      }
      if ((off_t)fread(romData.data(), 1, romSize, f) != romSize) {
        logE("launchRom: failed to read %s: %d %s\n", game_info->path, errno,
             strerror(errno));
        fclose(f);
        return 1;
      }
      fclose(f);
      game_info->data = romData.data();
      game_info->size = romData.size();
    }
    game_info->meta = "";
  }
  if (!priv->retro_load_game(&*game_info)) {
    logE("launchRom: noRom=%d, retro_load_game failed\n", (int)getNoRom());
    return 1;
  }
  return 0;
}

int RetroCore::resetCore() {
  priv->retro_reset();
  frameCount = 0;
  for (auto i = keyState.begin(); i != keyState.end(); i++) {
    *i = 0;
  }
  needChange = true;
  return 0;
}

int RetroCore::launchRom() {
  if (!priv || !systemInfo) {
    logE("launchRom: must call ctorError first\n");
    return 1;
  }
  setActiveCore(&*priv);
  if (!inited) {
    ports.clear();
    priv->retro_init();
    inited = true;
  }
  if (gameLoaded) {
    romData.clear();
    priv->retro_unload_game();
    gameLoaded = false;
  }

  if (loadRom()) {
    clearActiveCore();
    return 1;
  }
  gameLoaded = true;

  // get_system_av_info can only be called after retro_load_game.
  priv->retro_get_system_av_info(&priv->av_info);
  if (0) {
    logI("av_info.geometry.base %u x %u max %u x %u  aspect %.2f\n",
         priv->av_info.geometry.base_width, priv->av_info.geometry.base_height,
         priv->av_info.geometry.max_width, priv->av_info.geometry.max_height,
         priv->av_info.geometry.aspect_ratio);
    logI("av_info.timing.fps = %.1f sample_rate = %.1f\n",
         priv->av_info.timing.fps, priv->av_info.timing.sample_rate);
  }

  clearActiveCore();
  return resetCore() || mapInput();
}

int RetroCore::nextFrame(RetroUIinterface& ui) {
  if (!priv) {
    logE("nextFrame: must call ctorError first\n");
    return 1;
  }
  priv->ui = &ui;
  setActiveCore(&*priv);
  if (priv->earlyPerfLevel) {
    priv->earlyPerfLevel = false;
    ui.setPerfLevel(priv->perfLevel);
  }
  priv->retro_run();
  clearActiveCore();
  priv->ui = nullptr;
  frameCount++;
  return 0;
}

// environment is the way the core does "uncommon" tasks.
bool RetroCore::environment(unsigned cmd, void* data) {
  switch (cmd & (~RETRO_ENVIRONMENT_EXPERIMENTAL)) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      // Write true to data if video_refresh with data == NULL is allowed.
      *reinterpret_cast<bool*>(data) = true;
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
      auto* msg = reinterpret_cast<const struct retro_message*>(data);
      if (priv->ui) {
        priv->ui->showMessage(msg->msg, msg->frames);
      } else {
        logW("core posts %u frames of: \"%s\"\n", msg->frames, msg->msg);
      }
      return true;
    }
    case RETRO_ENVIRONMENT_SHUTDOWN:
      // This would, in theory, terminate the whole app. Ignore it.
      return true;
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
      // Called during retro_load_game(). If this system cannot achieve that
      // "level" as an emulator, say: the game may run poorly or not at all.
      if (!priv->ui) {
        priv->earlyPerfLevel = true;
        priv->perfLevel = *reinterpret_cast<const unsigned*>(data);
      } else {
        priv->ui->setPerfLevel(*reinterpret_cast<const unsigned*>(data));
      }
      return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      *reinterpret_cast<const char**>(data) = getSystemDir().c_str();
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return setPixelFormat((int)*(const enum retro_pixel_format*)data);
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      auto* var = reinterpret_cast<struct retro_variable*>(data);
      auto i = vars.find(var->key);
      if (i == vars.end()) {
        logW("get_var: \"%s\" = (not found)\n", var->key);
        var->value = NULL;
      } else {
        var->value = i->second.get();
      }
      return true;
    }
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
      return setInputDescriptors((const struct retro_input_descriptor*)data);
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
      auto* var = reinterpret_cast<const struct retro_variable*>(data);
      while (var->key) {
        if (!vars.emplace(std::make_pair(var->key, var)).second) {
          logW("RETRO_ENVIRONMENT_SET_VARIABLES: duplicate variable \"%s\"\n",
               var->key);
        }
        var++;
      }
      return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
      bool varsChanged{false};
      for (auto i = vars.begin(); i != vars.end(); i++) {
        if (i->second.changed) {
          varsChanged = true;
          i->second.changed = false;
        }
        if (i->second.current != 0 && needChange) {
          varsChanged = true;
        }
      }
      *reinterpret_cast<bool*>(data) = varsChanged;
      needChange = false;
      return true;
    }
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      noRom = *reinterpret_cast<const bool*>(data);
      return true;
    case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
      *reinterpret_cast<const char**>(data) = getCachePath().c_str();
      return true;
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
      memcpy(&priv->frame_time, (const struct retro_frame_time_callback*)data,
             sizeof(priv->frame_time));
      return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
      auto* cb = reinterpret_cast<struct retro_log_callback*>(data);
      cb->log = core_private::log_printf;
      return true;
    }
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *reinterpret_cast<const char**>(data) = getSavePath().c_str();
      return true;
    case RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK:
      priv->get_proc_address =
          reinterpret_cast<struct retro_get_proc_address_interface*>(data)
              ->get_proc_address;
      return true;
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
      priv->addSubsystem((const struct retro_subsystem_info*)data);
      return true;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
      addControllerArray((const struct retro_controller_info*)data);
      return true;
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
      auto* map = reinterpret_cast<const struct retro_memory_map*>(data);
      priv->memmap.insert(priv->memmap.begin(), &map->descriptors[0],
                          &map->descriptors[map->num_descriptors]);
      return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
      const struct retro_game_geometry* geom =
          reinterpret_cast<const struct retro_game_geometry*>(data);
      // Video framebuffer size is changed inside video_refresh. Thanks, but
      // since the geometry will show up there, just copy the values here.
      priv->av_info.geometry.base_width = geom->base_width;
      priv->av_info.geometry.base_height = geom->base_height;
      priv->av_info.geometry.aspect_ratio = geom->aspect_ratio;
      return true;
    }
    case RETRO_ENVIRONMENT_GET_USERNAME:
      *reinterpret_cast<const char**>(data) = NULL;
      return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS&(
        ~RETRO_ENVIRONMENT_EXPERIMENTAL):
      if (*reinterpret_cast<const bool*>(data)) {
        // This core can provide "achievements" data.
      }
      return true;
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
      setupSerializer(*reinterpret_cast<uint64_t*>(data));
      return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE&(
        ~RETRO_ENVIRONMENT_EXPERIMENTAL):
      // 1:enable video 2:enable audio 4:fast savestates 8:hard disable audio
      *reinterpret_cast<int*>(data) = 1 | 2;
      return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
      *reinterpret_cast<unsigned*>(data) = 0;
      return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS&(~RETRO_ENVIRONMENT_EXPERIMENTAL):
      return false;

    case RETRO_ENVIRONMENT_SET_ROTATION:  // No known core uses this.
    case RETRO_ENVIRONMENT_GET_OVERSCAN:  // Deprecated.
    case 4:                               // cmd removed.
    case 5:                               // cmd removed.
    case 20:                              // cmd removed.
    default:
      if (cmd & RETRO_ENVIRONMENT_EXPERIMENTAL) {
        logW("core wants cmd (%u | RETRO_ENVIRONMENT_EXPERIMENTAL)\n",
             cmd & (~RETRO_ENVIRONMENT_EXPERIMENTAL));
      } else {
        logW("core wants cmd  %u  (unsupported)\n", cmd);
      }
      return false;
  }
}

bool RetroCore::setPixelFormat(int fmt) {
  switch ((enum retro_pixel_format)fmt) {
    case RETRO_PIXEL_FORMAT_XRGB8888:  // GL_BGRA GL_UNSIGNED_INT_8_8_8_8_REV
      // NOTE: You should check if device supports _SRGB, if not then use
      // _UNORM.
      pixelFmt = VK_FORMAT_R8G8B8A8_SRGB;
      return true;
    case RETRO_PIXEL_FORMAT_0RGB1555:  // GL_BGRA GL_UNSIGNED_SHORT_1_5_5_5_REV
      pixelFmt = VK_FORMAT_A1R5G5B5_UNORM_PACK16;
      return true;
    case RETRO_PIXEL_FORMAT_RGB565:  // GL_RGB GL_UNSIGNED_SHORT_5_6_5
      pixelFmt = VK_FORMAT_B5G6R5_UNORM_PACK16;
      return true;
    default:
      logE("Unknown retro_pixel_format %d\n", fmt);
      return false;
  }
}

void RetroCore::setupSerializer(uint64_t& quirks) {
  serializeQuirks = quirks;
  // Only keep the flags supported by this implementation.
  serializeQuirks &= RETRO_SERIALIZATION_QUIRK_MUST_INITIALIZE |
                     RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE |
                     RETRO_SERIALIZATION_QUIRK_SINGLE_SESSION |
                     RETRO_SERIALIZATION_QUIRK_ENDIAN_DEPENDENT;
  serializeQuirks |= RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE;
  quirks = serializeQuirks;
}

int RetroCore::save(std::vector<char>& dst) {
  wasLastSaveTooEarly = false;
  if (serializeQuirks & RETRO_SERIALIZATION_QUIRK_SINGLE_SESSION) {
    logW("retro_serialize: core says SINGLE_SESSION. Refusing to save.\n");
    return 1;
  }

  setActiveCore(&*priv);
  size_t saveDataLen = priv->retro_serialize_size();
  if (saveDataLen > maxSaveDataLen) {
    logW("retro_serialize_size %llx larger than %llx\n",
         (unsigned long long)saveDataLen, (unsigned long long)maxSaveDataLen);
    clearActiveCore();
    return 1;
  }
  dst.resize(protoSize(saveDataLen));
  auto* p = reinterpret_cast<RetroSaveProto*>(dst.data());
  memset(p, 0, dst.size());
  memcpy(p->magic, RETROSAVEPROTO_MAGIC, sizeof(p->magic));
  p->saveDataLen = saveDataLen;
  if (htonl(1) != 1) {   // If true, this machine is not big-endian.
    p->flags |= 1 << 4;  // Assume it is also not PDP-endian.
  }
  p->flags |= serializeQuirks & RETRO_SERIALIZATION_QUIRK_ENDIAN_DEPENDENT;
  p->flags |= serializeQuirks & RETRO_SERIALIZATION_QUIRK_PLATFORM_DEPENDENT;
  p->flags |= RETROSAVEPROTO_ALWAYS_FLAGS;
  if (!priv->retro_serialize(&p->saveData, saveDataLen)) {
    clearActiveCore();
    if (serializeQuirks & RETRO_SERIALIZATION_QUIRK_MUST_INITIALIZE) {
      // This quirk affects a core before it has run for very many frames.
      if (frameCount < 30) {
        logW("retro_serialize failed at startup: %zu frames\n", frameCount);
        wasLastSaveTooEarly = true;
        return 1;
      }
    }
    logW("retro_serialize failed\n");
    return 1;
  }
  clearActiveCore();
  // If a retro_serialize call succeeds, this quirk no longer applies.
  serializeQuirks &= ~RETRO_SERIALIZATION_QUIRK_MUST_INITIALIZE;
  return 0;
}

int RetroCore::load(const std::vector<char>& src) {
  if (src.size() < protoSize(0)) {
    logW("save corrupt: got %zu bytes\n", src.size());
  }
  auto* p = reinterpret_cast<const RetroSaveProto*>(src.data());
  if (memcmp(p->magic, RETROSAVEPROTO_MAGIC, sizeof(p->magic))) {
    logW("save magic: got \"%.*s\" want \"%.*s\"\n", (int)sizeof(p->magic),
         p->magic, (int)sizeof(p->magic), RETROSAVEPROTO_MAGIC);
    return 1;
  }
  if (p->saveDataLen < 4 || p->saveDataLen > maxSaveDataLen ||
      protoSize(p->saveDataLen) != src.size()) {
    logW("save data: got len=%x want %x\n", p->saveDataLen,
         (uint32_t)(src.size() - protoSize(0)));
    return 1;
  }
  if ((p->flags & RETROSAVEPROTO_ALWAYS_FLAGS) != RETROSAVEPROTO_ALWAYS_FLAGS) {
    logW("save invalid: got %llx want %llx\n",
         (unsigned long long)(p->flags & RETROSAVEPROTO_ALWAYS_FLAGS),
         (unsigned long long)RETROSAVEPROTO_ALWAYS_FLAGS);
    return 1;
  }
  if (p->flags & RETRO_SERIALIZATION_QUIRK_ENDIAN_DEPENDENT) {
    // Check if this machine matches the type in the flags.
    uint64_t wantFlags = (htonl(1) != 1) ? (1 << 4) : 0;
    if ((p->flags & (1 << 4)) != wantFlags) {
      logW("save arch: got %llx want %llx\n",
           (unsigned long long)(p->flags & (1 << 4)),
           (unsigned long long)wantFlags);
      return 1;
    }
  }
  if (p->flags & RETRO_SERIALIZATION_QUIRK_PLATFORM_DEPENDENT) {
    logW("save says platform dependent. not implemented yet.\n");
    return 1;
  }
  setActiveCore(&*priv);
  if (!priv->retro_unserialize(&p->saveData, p->saveDataLen)) {
    clearActiveCore();
    logW("retro_unserialize failed\n");
    // Reset core to get it to a known state.
    resetCore();
    return 1;
  }
  clearActiveCore();
  return 0;
}
