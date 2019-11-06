/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This stores downloaded files in a cache.
 */

#include <errno.h>
#include <sys/stat.h>   // for mkdir(), stat()
#include <sys/types.h>  // for mkdir(), stat(), opendir()

#include "retroweb.h"
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir(x, y) _mkdir(x)
#define OS_SEP '\\'
#else                /*_WIN32*/
#include <dirent.h>  // for opendir()
#include <unistd.h>  // for stat()
#define OS_SEP '/'
#ifdef __ANDROID__
#include <android_native_app_glue.h>
#endif /*__ANDROID__*/
#endif /*_WIN32*/

#include <string>
#include <vector>

#include "retrojni.h"

static int mkdirP(const char* dirname) {
  const char* parent = strrchr(dirname, OS_SEP);
  if (parent != NULL) {
    std::string s(dirname);
    s.erase(s.begin() + (parent - dirname), s.end());
    if (s.empty()) {
      logE("mkdir(%s): already exists\n", dirname);
      return 1;
    }
    struct stat st;
    if (stat(s.c_str(), &st)) {
      if (errno == ENOENT) {
        // Attempt to create the parent dir recursively.
        if (mkdirP(s.c_str())) {
          logE("mkdir(%s) failed because mkdir(%s) failed\n", dirname,
               s.c_str());
          return 1;
        }
      } else {
        logE("stat(%s) failed: %d %s\n", s.c_str(), errno, strerror(errno));
        return 1;
      }
    } else if (!(st.st_mode & S_IFDIR)) {
      logE("mkdir(%s): %s is not a dir\n", dirname, s.c_str());
      return 1;
    }
  }
  if (mkdir(dirname, 511)) {
    if (errno == EEXIST) {
      // Success! Well, it's what was wanted, anyway.
      return 0;
    }
    logE("mkdir(%s, 0777) failed: %d %s\n", dirname, errno, strerror(errno));
    return 1;
  }
  return 0;
}

static int chooseFirstPath(std::string& out, const char* paths,
                           const char* fallback) {
  const char* first = paths;
  while (first && *first) {
    const char* sep = strchr(first, ':');
    size_t n;
    if (!sep) {
      n = strlen(first);
    } else {
      n = sep - first;
      sep++;  // When first = sep happens, below, point after the separator.
    }
    if (n > 0) {  // Ignore entry if it is empty.
      out.assign(first, n);
      struct stat st;
      if (!stat(out.c_str(), &st)) {
        // File or directory found.
        if (!(st.st_mode & S_IFDIR)) {
          logW("chooseFirstPath: %s is not a dir\n", out.c_str());
        } else {
          return 0;
        }

        // Keep searching if stat reports ENOENT.
      } else if (errno != ENOENT) {
        // Fail now if stat reports some other error.
        logE("chooseFirstPath(%s): stat(%s) failed: %d %s\n", paths,
             out.c_str(), errno, strerror(errno));
        return 1;
      }
    }
    first = sep;
  }

  // No valid paths found. Use 'fallback'.
#ifdef _WIN32
  const char* home = getenv("USERPROFILE");
#else
  const char* home = getenv("HOME");
#endif
  if (fallback[0] == '~' && fallback[1] == OS_SEP && home) {  // Replace ~.
    out = home;
    fallback++;
    out += fallback;
  } else {
    out = fallback;
  }
  return 0;
}

extern struct android_app* OPENSSL_android_native_app;

int RetroWeb::initCache() {
  buildbotUrl = "http://buildbot.libretro.com/nightly/";
  // Platform-specific build.
#ifdef _WIN32
  const char* home = getenv("USERPROFILE");
  if (!home) {
    logE("RetroWeb::ctorError: env[USERPROFILE] not found\n");
    return 1;
  }
  cachePath = home;
  cachePath += "\\AppData\\Local";
  savePath = cachePath;
  savePath += "\\retroweb\\retrogame\\save";
  soExt = "dll";
  buildbotPlatform = "windows-msvc2010/";
#ifdef _M_X64
  buildbotArch = "x86_64/";
#elif defined(_M_IX86)
  buildbotArch = "x86/";
#else
#error Unsupported win32 platform
#endif
#elif defined(__APPLE__)
  // cachePath is initialized below.
  soExt = "dylib";
  buildbotPlatform = "apple/osx/";
#ifdef __x86_64__
  buildbotArch = "x86_64/";
#elif defined(__i386__)
  buildbotArch = "x86/";
#else
#error Unsupported apple platform
#endif
#elif defined(__ANDROID__)
  if (RetroWeb_checkPlatform()) {
    logE("RetroWeb::ctorError: checkPlatform failed\n");
    return 1;
  }
  struct android_app* app = glfwGetAndroidApp();
  cachePath = app->activity->internalDataPath;
  OPENSSL_android_native_app = app;
  logW("TODO: use activity->externalDataPath\n");
  logW("TODO: use activity->obbPath\n");

  soExt = "so";
  buildbotPlatform = "android/";
  savePath = cachePath;
  savePath += "/save";
#ifdef __x86_64__
  buildbotArch = "x86_64/";
#elif defined(__i386__)
  buildbotArch = "x86/";
#elif defined(__aarch64__)
  buildbotArch = "arm64-v8a/";
#elif (defined(__arm__) || defined(_M_ARM) || defined(_M_ARMT)) && \
    defined(__ARM_ARCH_7A__)
  buildbotArch = "armeabi-v7a/";
#elif (defined(__arm__) || defined(_M_ARM) || defined(_M_ARMT)) && \
    !defined(__ARM_ARCH_7A__)
  buildbotArch = "armeabi/";
#else
#error Unsupported android platform
#endif
#elif defined(__linux__)
  // cachePath is initialized below.
  soExt = "so";
  buildbotPlatform = "linux/";
#ifdef __x86_64__
  buildbotArch = "x86_64/";
#elif defined(__i386__)
  buildbotArch = "x86/";
#elif defined(__aarch64__)
  buildbotArch = "armv7-neon-hf/";
#elif (defined(__arm__) || defined(_M_ARM) || defined(_M_ARMT)) && \
    (defined(__ARM_NEON) || defined(__ARM_NEON__))
  buildbotArch = "armv7-neon-hf/";
#elif (defined(__arm__) || defined(_M_ARM) || defined(_M_ARMT)) && \
    !(defined(__ARM_NEON) || defined(__ARM_NEON__))
  buildbotArch = "armhf/";
#else
#error Unsupported linux platform
#endif
#else
#error Unsupported OS
#endif

#if defined(__APPLE__) || (defined(__linux__) && !defined(__ANDROID__))
  // NOTE Android sets savePath above. This is for linux and macOS platforms.
  if (chooseFirstPath(savePath, getenv("XDG_CONFIG_HOME"), "~/.config")) {
    logE("RetroWeb::ctorError: savePath failed\n");
    return 1;
  }
  if (savePath.at(savePath.size() - 1) != OS_SEP) {
    savePath += OS_SEP;
  }
  savePath += "retrogame/save";
  if (chooseFirstPath(cachePath, getenv("XDG_CACHE_HOME"), "~/.cache")) {
    logE("RetroWeb::ctorError: cachePath failed\n");
    return 1;
  }
#endif
  gamePath = savePath;
  auto pos = gamePath.find("save");
  if (pos != std::string::npos) {
    gamePath.erase(pos);
  }
  gamePath += "game";
  systemDir = cachePath;
  systemDir = ((systemDir + OS_SEP) + "retroweb" + OS_SEP) + "sys";

  // Add retroweb-specific parts to cachePath.
  cachePath += OS_SEP;
  cachePath += "retroweb";
  cachePath += OS_SEP;
  cachePath += "core";
  if (mkdirP(cachePath.c_str())) {
    logE("RetroWeb::ctorError: mkdir(%s) failed\n", cachePath.c_str());
    return 1;
  }
  if (mkdirP(systemDir.c_str())) {
    logE("RetroWeb::ctorError: mkdir(%s) failed\n", systemDir.c_str());
    return 1;
  }
  if (mkdirP(savePath.c_str())) {
    logE("RetroWeb::ctorError: mkdir(%s) failed\n", savePath.c_str());
    return 1;
  }
  if (mkdirP(gamePath.c_str())) {
    logE("RetroWeb::ctorError: mkdir(%s) failed\n", gamePath.c_str());
    return 1;
  }
#ifdef __ANDROID__
  logI("cachePath=%s\n", cachePath.c_str());
  logI("systemDir=%s\n", systemDir.c_str());
  logI("savePath=%s\n", savePath.c_str());
  logI("gamePath=%s\n", gamePath.c_str());
#endif
  return 0;
}

#ifdef _WIN32
int RetroWeb::createTmpdir() {
  char syncTemplate[256];
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  srand((unsigned)qpc.LowPart);
  strcpy(syncTemplate, "retroweb.");
  size_t len = strlen(syncTemplate);
  static const char base64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-";
  for (int i = 0; i < 6; i++) {
    syncTemplate[len++] = base64[rand() % 64];
  }
  syncTemplate[len] = 0;
  do {
    const char* envTmp = getenv("TEMP");
    if (!envTmp) {
      envTmp = getenv("TMP");
      if (!envTmp) {
        envTmp = getenv("USERPROFILE");
        if (!envTmp) {
          logE("createTmpdir: TEMP, TMP and USERPROFILE not found\n");
          return 1;
        }
        syncTmpdir = envTmp;
        syncTmpdir += "\\AppData\\Local\\Temp\\";
        syncTmpdir += syncTemplate;
        break;
      }
    }
    syncTmpdir = envTmp;
    syncTmpdir += '\\';
    syncTmpdir += syncTemplate;
  } while (0);
  if (mkdirP(syncTmpdir.c_str())) {
    logE("createTmpDir(%s): mkdirP failed\n", syncTmpdir.c_str());
    return 1;
  }
  return 0;
}

struct dirent {
  char* d_name;
};

struct DIR {
  ptrdiff_t handle;  // handle is -1 for failed rewind.
  struct _finddata_t info;
  struct dirent result;  // d_name is null the first time.
  char* name;
};

DIR* opendir(const char* name) {
  if (!name || !name[0]) {
    errno = EINVAL;
    return NULL;
  }
  DIR* dir = 0;
  size_t base_length = strlen(name);
  const char* all = /* search pattern must end with suitable wildcard */
      strchr("/\\", name[base_length - 1]) ? "*" : "/*";

  dir = (DIR*)malloc(sizeof *dir);
  if (!dir) {
    errno = ENOMEM;
    return NULL;
  }
  base_length += strlen(all) + 1;
  dir->name = (char*)malloc(base_length);
  if (!dir->name) {
    free(dir);
    errno = ENOMEM;
    return NULL;
  }
  snprintf(dir->name, base_length, "%s%s", name, all);

  dir->handle = (ptrdiff_t)_findfirst(dir->name, &dir->info);
  if (dir->handle == -1) {
    free(dir->name); /* rollback */
    free(dir);
    return NULL;
  }
  dir->result.d_name = 0;
  return dir;
}

int closedir(DIR* dir) {
  if (!dir || dir->handle == -1) {
    errno = EBADF;
    return -1;
  }

  free(dir->name);
  if (_findclose(dir->handle) < 0) {
    errno = EBADF;
    return -1;
  }
  free(dir);
  return 0;
}

struct dirent* readdir(DIR* dir) {
  if (!dir || dir->handle == -1) {
    errno = EBADF;
    return NULL;
  }
  struct dirent* r = &dir->result;
  if (dir->result.d_name && _findnext(dir->handle, &dir->info) == -1) {
    errno = EBADF;
    return NULL;
  }
  r->d_name = dir->info.name;
  return r;
}

int recursiveDeleteDir(const char* path) {
  DIR* dp = opendir(path);
  if (!dp) {
    logE("recursiveDeleteDir(%s): %d %s\n", path, errno, strerror(errno));
    return 1;
  }
  struct dirent* ent;
  while ((ent = readdir(dp))) {
    if (!strcmp(ent->d_name, "..") || !strcmp(ent->d_name, ".")) {
      continue;
    }
    std::string fullpath(path);
    fullpath += OS_SEP;
    fullpath += ent->d_name;
    struct stat st;
    if (stat(fullpath.c_str(), &st)) {
      logE("recursiveDeleteDir: stat %s %d %s\n", fullpath.c_str(), errno,
           strerror(errno));
      return 1;
    }
    if (st.st_mode & S_IFDIR) {
      if (recursiveDeleteDir(fullpath.c_str())) {
        logE("recursiveDeleteDir(%s): failed in this dir\n", path);
        return 1;
      }
    } else {
      if (unlink(fullpath.c_str())) {
        logE("recursiveDeleteDir: rm %s %d %s\n", fullpath.c_str(), errno,
             strerror(errno));
        return 1;
      }
    }
  }
  closedir(dp);
  if (_rmdir(path)) {
    logE("recursiveDeleteDir(%s): final _rmdir failed\n", path);
    return 1;
  }
  return 0;
}
#else /*_WIN32*/
int RetroWeb::createTmpdir() {
#ifdef __ANDROID__
  syncTmpdir = cachePath;
  syncTmpdir += "/tXXXXXX";
#else  /* __ANDROID__ */
  syncTmpdir = "/tmp/retroweb.XXXXXX";
#endif /* __ANDROID__ */
  if (!mkdtemp(const_cast<char*>(syncTmpdir.c_str()))) {
    snprintf(syncStatus, sizeof(syncStatus),
             "startSync: mkdtemp failed: %d %s\n", errno, strerror(errno));
    return 1;
  }
  return 0;
}
#endif /*_WIN32*/

static int copyFile(const std::string& src, std::string dst) {
  FILE* fsrc = fopen(src.c_str(), "rb");
  if (!fsrc) {
    logE("open(%s) failed: %d %s\n", src.c_str(), errno, strerror(errno));
    return 1;
  }
  FILE* fdst = fopen(dst.c_str(), "wb");
  if (!fdst) {
    logE("open(%s) failed: %d %s\n", dst.c_str(), errno, strerror(errno));
    return 1;
  }
  char buf[4096];
  while (!feof(fsrc)) {
    size_t n = fread(buf, 1, sizeof(buf), fsrc);
    if (n < 1) {
      if (feof(fsrc)) {
        break;
      }
      logE("read(%s) failed: %d %s\n", src.c_str(), errno, strerror(errno));
      return 1;
    }
    if (fwrite(buf, 1, n, fdst) != n) {
      logE("copy(%s) failed: %d %s\n", dst.c_str(), errno, strerror(errno));
      return 1;
    }
  }
  fclose(fsrc);
  fclose(fdst);
  return 0;
}

int RetroWeb::listCacheContents() {
  DIR* dp = opendir(cachePath.c_str());
  if (!dp) {
    logE("opendir(%s): %d %s\n", cachePath.c_str(), errno, strerror(errno));
    return 1;
  }
  struct dirent* ent;
  while ((ent = readdir(dp))) {
    if (!strcmp(ent->d_name, "..") || !strcmp(ent->d_name, ".")) {
      continue;
    }
    std::string fullpath(cachePath);
    fullpath += OS_SEP;
    fullpath += ent->d_name;
    struct stat st;
    if (stat(fullpath.c_str(), &st)) {
      logE("opendir(%s): %s %d %s\n", cachePath.c_str(), fullpath.c_str(),
           errno, strerror(errno));
      return 1;
    }
    if (st.st_mode & S_IFDIR) {
      logW("opendir(%s): ignore subdir %s\n", cachePath.c_str(),
           fullpath.c_str());
      continue;
    }
    if (addToCores(fullpath.c_str())) {
      return 1;
    }
  }
  closedir(dp);

  std::vector<std::string> gameSearchPaths{gamePath};
#ifdef __ANDROID__
  gameSearchPaths.push_back("/sdcard");
#endif /*__ANDROID__*/
  for (auto path : gameSearchPaths) {
    dp = opendir(path.c_str());
    if (!dp) {
      if (path != gamePath) {
        continue;
      }
      logE("opendir(%s): %d %s\n", path.c_str(), errno, strerror(errno));
      return 1;
    }
    while ((ent = readdir(dp))) {
      if (!strcmp(ent->d_name, "..") || !strcmp(ent->d_name, ".")) {
        continue;
      }
      std::string fullpath(path);
      fullpath += OS_SEP;
      fullpath += ent->d_name;
      struct stat st;
      if (stat(fullpath.c_str(), &st)) {
        logE("stat(%s) failed: %d %s\n", fullpath.c_str(), errno,
             strerror(errno));
        return 1;
      }
      if (st.st_mode & S_IFDIR) {
        logW("%s: ignore subdir\n", fullpath.c_str());
        continue;
      }
      std::string t = getFileType(fullpath, st.st_size);
      if (t.empty() || t.at(0) == '(') {
        if (t.empty()) {
          t = "(unknown error)";
        }
        logW("ignore %s: %s\n", fullpath.c_str(), t.c_str());
        continue;
      }
      auto r = typeMap.find(t);
      if (r == typeMap.end()) {
        logW("ignore %s: type %s not found\n", fullpath.c_str(), t.c_str());
        continue;
      }
      if (r->second.empty()) {
        logW("ignore %s: BUG: type %s with no cores?\n", fullpath.c_str(),
             t.c_str());
        continue;
      }
      auto c = cores.find(r->second.at(0));
      if (c == cores.end()) {
        logW("ignore %s: BUG: cannot find a core \"%s\"\n", fullpath.c_str(),
             r->second.at(0).c_str());
        continue;
      }
      RetroApp app(c->second, t);
      if (app.load(fullpath, st.st_size, ent->d_name)) {
        logE("load(%s) failed\n", fullpath.c_str());
        continue;
      }
      auto added = apps.emplace(app.getName(), app);
      if (added.second && path != gamePath) {
        // TODO: find a better way to transfer data into internal storage.
        // adb backup and adb restore?
        std::string dst = (gamePath + OS_SEP) + ent->d_name;
        if (copyFile(fullpath, dst)) {
          logE("copyFile(%s) failed\n", dst.c_str());
          return 1;
        }
        logE("copyFile(%s) OK\n", dst.c_str());
      }
    }
    closedir(dp);
  }

  dp = opendir(getSavePath().c_str());
  if (!dp) {
    logE("opendir(%s): %d %s\n", getSavePath().c_str(), errno, strerror(errno));
    return 1;
  }
  while ((ent = readdir(dp))) {
    if (!strcmp(ent->d_name, "..") || !strcmp(ent->d_name, ".")) {
      continue;
    }
    std::string fullpath(getSavePath());
    fullpath += OS_SEP;
    fullpath += ent->d_name;
    struct stat st;
    if (stat(fullpath.c_str(), &st)) {
      logE("stat(%s) failed: %d %s\n", fullpath.c_str(), errno,
           strerror(errno));
      return 1;
    }
    if (st.st_mode & S_IFDIR) {
      logW("%s: ignore subdir\n", fullpath.c_str());
      continue;
    }
    std::string name = ent->d_name;
    auto pos = name.rfind(".");
    if (pos != std::string::npos && name.substr(pos + 1) == "bin") {
      saves.emplace(name.substr(0, pos));
    }
  }

  haveNstDatabase = false;
  dp = opendir(getSystemDir().c_str());
  if (!dp) {
    logE("opendir(%s): %d %s\n", getSystemDir().c_str(), errno,
         strerror(errno));
    return 1;
  }
  while ((ent = readdir(dp))) {
    if (!strcmp(ent->d_name, "..") || !strcmp(ent->d_name, ".")) {
      continue;
    }
    std::string fullpath(getSystemDir());
    fullpath += OS_SEP;
    fullpath += ent->d_name;
    struct stat st;
    if (stat(fullpath.c_str(), &st)) {
      logE("stat(%s) failed: %d %s\n", fullpath.c_str(), errno,
           strerror(errno));
      return 1;
    }
    if (st.st_mode & S_IFDIR) {
      continue;
    }
    if (!strcmp(ent->d_name, "NstDatabase.xml")) {
      haveNstDatabase = true;
    }
  }
  return 0;
}

int RetroWeb::addToCores(const char* pathname) {
  auto p = cores.emplace(pathname, RetroCore(*this, pathname));
  if (!p.second) {
    // duplicate core
    return 0;
  }
  auto& c = p.first->second;
  if (c.ctorError()) {
    return 1;
  }
  std::string firstExt;
  for (auto& oneExt : c.getSupport()) {
    if (firstExt.empty()) {
      firstExt = oneExt;
    }
    auto t = typeMap.emplace(oneExt, std::vector<std::string>{});
    t.first->second.emplace_back(c.getCorepath());
  }
  if (c.getNoRom()) {
    auto r = apps.emplace(c.getName(), RetroApp(c, firstExt));
    if (!r.second) {
      logE("addToCores(%s): has no rom, but dup app\n", pathname);
      return 1;
    }
  }
  return 0;
}
