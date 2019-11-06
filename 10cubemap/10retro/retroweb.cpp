/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This accesses http://buildbot.libretro.com
 */

#include "retroweb.h"

#include <errno.h>
#include <minizip/unzip.h>
#include <minizip/zip.h>
#include <src/core/VkPtr.h>
#include <sys/stat.h>   // for stat()
#include <sys/types.h>  // for stat()

#include <set>
#ifdef _WIN32
#include <direct.h>  // for _rmdir()

#include <chrono>  // for std::chrono
#include <thread>  // for std::this_thread::sleep_for
#define OS_SEP '\\'

int recursiveDeleteDir(const char* path);
#else
#include <unistd.h>  // for unlink(), rmdir()
#define OS_SEP '/'
#endif

RetroWeb::~RetroWeb() { cleanup(); }

int RetroWeb::ctorError() {
  if (initCache()) {
    return 1;
  }

  // Initialize cache index.
  if (listCacheContents()) {
    logE("RetroWeb: listCacheContents failed\n");
    return 1;
  }
  if (http.ctorError()) {
    logE("RetroWeb: RetroHttp.ctorError failed\n");
    return 1;
  }
  http.setDataCallback(this, dataCallback);
  snprintf(syncStatus, sizeof(syncStatus), "idle");
  return 0;
}

int RetroWeb::stop() {
  http.reset();
  if (syncRunning) {
    cleanup();
    syncGetRunning = false;
    syncRunning = false;
  }
  if (http.ctorError()) {
    logE("RetroWeb: RetroHttp.ctorError failed\n");
    return 1;
  }
  return 0;
}

// gcc 7.1 and higher warn when snprintf may truncate output.
// This tells the compiler the truncation is known and acceptable.
static void ignore_truncated() {}

int RetroWeb::poll() {
  if (syncRunning) {
    if (syncGetRunning) {
      syncGetRunning = http.pollGET(syncProgress);
      if (!syncGetRunning) {
        if (syncOutf) {
          fclose(syncOutf);
          syncOutf = NULL;
        }
      }
    } else {
      http.removeGET();
      if (!getLastSyncError()) {
        syncRunning = 0;
        if (syncTmpdir.empty()) {
          // File is not in a tmpdir. Do not delete it, keep it.
          if (syncOutfilename.find("NstDatabase.xml") != std::string::npos) {
            haveNstDatabase = true;
          }
          syncOutfilename.clear();
        } else if (readDownload()) {
          char syncCopy[sizeof(syncStatus)];
          if (snprintf(syncCopy, sizeof(syncCopy), "readDownload(%s): %s",
                       syncOutfilename.c_str(), syncStatus) < 0)
            ignore_truncated();
          strcpy(syncStatus, syncCopy);
          logW("%s\n", syncCopy);
          cleanup();
          return 0;
        }
        cleanup();
        snprintf(syncStatus, sizeof(syncStatus), "idle");
        if (listCacheContents()) {
          logE("poll: listCacheContents failed\n");
          return 1;
        }
        if (startSync()) {
          logE("poll: startSync failed\n");
          return 1;
        }
      }
    }
  }
  return 0;  // Any http error is not an error here.
}

int RetroWeb::dataCallback(void* self, void* data, size_t len, size_t maxlen) {
  (void)maxlen;
  if (fwrite(data, 1, len, static_cast<RetroWeb*>(self)->syncOutf) != len) {
    logE("dataCallback: fwrite(%s) failed: %d %s\n",
         static_cast<RetroWeb*>(self)->syncOutfilename.c_str(), errno,
         strerror(errno));
    return 1;
  }
  return 0;
}

void RetroWeb::cleanup() {
  if (syncOutf) {
    fclose(syncOutf);
    syncOutf = NULL;
  }
  if (!syncOutfilename.empty()) {
    if (unlink(syncOutfilename.c_str())) {
      logE("unlink(%s) failed: %d %s\n", syncOutfilename.c_str(), errno,
           strerror(errno));
      if (!syncTmpdir.empty()) {
        logE("syncTmpdir not deleted: %s\n", syncTmpdir.c_str());
      }
      return;
    }
    syncOutfilename.clear();
  }
  if (!syncTmpdir.empty()) {
#ifdef _WIN32
    if (recursiveDeleteDir(syncTmpdir.c_str())) {
      logE("recursiveDeleteDir(%s) failed\n", syncTmpdir.c_str());
    }
#else  /*_WIN32*/
    if (rmdir(syncTmpdir.c_str())) {
      logE("rmdir(%s) failed: %d %s\n", syncTmpdir.c_str(), errno,
           strerror(errno));
    }
#endif /*_WIN32*/
    syncTmpdir.clear();
  }
}

std::string RetroWeb::getUrlFor(std::string name) const {
#ifdef _WIN32
  if (name == "snes9x") {
    name = "snes9x2010";
  }
#endif /*_WIN32*/
  return buildbotUrl +
#ifdef __ANDROID__
         // buildbot Android layout is different than anything else. The arch
         // subdir is inside the date subdir, instead of the other way around.
         buildbotPlatform + "latest/" + buildbotArch + name +
         "_libretro_android." +
#else
         buildbotPlatform + buildbotArch + "latest/" + name + "_libretro." +
#endif
         soExt + ".zip";
}

int RetroWeb::startSync() {
  http.clearError();

  // TODO: Also sync with online:
  /*
curl -H "User-Agent: libretro" http://buildbot.libretro.com | \
awk '{t = $0;
while (1) {
  m = match(t, "<a href=\"");
  if (m == 0) break;
  t = substr(t,m + 9);
  n = match(t, "\"[^>]*>");
  if (n) {
    u = substr(t, 1, n - 1);
    t = substr(t, n);
    if (u !~ "^http" && u !~ "\.php$" && u !~ "\.mp4") {
      print u;
    }
  }
}}'
  */
  std::set<std::string> wantCores = {
      "2048", "nestopia", "stella",
      "snes9x",  // see https://project.satellaview.org/downloads.htm for BS-X
  };
  for (auto& kv : cores) {
    std::string name = kv.second.getName();
#ifdef _WIN32
    if (name == "snes9x 2010") {
      name = "snes9x";
    }
#endif /*_WIN32*/
    auto i = wantCores.find(name);
    if (i != wantCores.end()) {
      wantCores.erase(i);
    }
  }
  std::string url;
  if (!wantCores.empty()) {
    if (createTmpdir()) {
      return 1;
    }
    url = getUrlFor(*wantCores.begin());
    syncOutfilename = syncTmpdir + OS_SEP + "z.zip";
  } else if (haveNstDatabase) {
    return 0;
  } else {
    url =
        "https://raw.githubusercontent.com/"
        "libretro/nestopia/master/NstDatabase.xml";
    syncOutfilename = getSystemDir() + OS_SEP + "NstDatabase.xml";
  }

  // http://buildbot.libretro.com/nightly/android/latest/arm64-v8a/2048_libretro_android.so.zip
  //    http://buildbot.libretro.com/nightly/apple/osx/x86_64/latest/2048_libretro.dylib.zip
  /*
  Finish writing RetroWeb:
  * kick off another GET request after getting the first one
  * unzip on another thread
  * CORE_TYPE_ - see menu/cbs/menu_cbs_ok.c
    (would have to statically know of a core of the right CORE_TYPE_
     to render content that is known to be media - e.g. ffmpeg, image -
     need a built-in mapping of file type -> core location + CORE_TYPE_*)
  * some good icons in media/assets/pkg/wiiu
  * some good icons in media/assets/src/xmb (svg, png in ../xmb)
  */
  syncOutf = fopen(syncOutfilename.c_str(), "wb");
  if (!syncOutf) {
    if (snprintf(syncStatus, sizeof(syncStatus),
                 "startSync: fopen(%s): %d %s\n", syncOutfilename.c_str(),
                 errno, strerror(errno)) < 0)
      ignore_truncated();
    return 1;
  }
  if (http.startGET(url.c_str())) {
    snprintf(syncStatus, sizeof(syncStatus),
             "startSync: RetroHttp.startGET failed\n");
    return 1;
  }
  const char* urlPath = http.getUrlPath();
  if (!urlPath) {
    snprintf(syncStatus, sizeof(syncStatus), "startSync: getUrlPath failed\n");
    return 1;
  }
  if (urlPath[0] == '/') {
    urlPath++;
  }
  strncpy(shortFilename, urlPath, sizeof(shortFilename));
  syncRunning = 1;
  syncGetRunning = 1;
  return 0;
}

int RetroWeb::getLastSyncError() const { return http.getErrno(); }

const char* RetroWeb::getSyncStatus() {
  if (!syncRunning) {
    return syncStatus;
  }

  if (syncGetRunning) {
    if (snprintf(syncStatus, sizeof(syncStatus), "%5.1f%% %s",
                 syncProgress * 100.f, shortFilename) < 0)
      ignore_truncated();
    return syncStatus;
  }

  int httpErrno = http.getErrno();
  if (httpErrno == RetroHttp::RETRO_NO_ERROR) {
    if (!syncTmpdir.empty()) {
      snprintf(syncStatus, sizeof(syncStatus), "unzip");
    }
  } else if (httpErrno == RetroHttp::FAIL_VIA_HTTP_STATUS) {
    snprintf(syncStatus, sizeof(syncStatus), "http/%ld", http.getStatusCode());
  } else {
    snprintf(syncStatus, sizeof(syncStatus), "%s", http.getError());
  }
  return syncStatus;
}

static void unzErrToString(int unzRet, char* buf, size_t bufSize) {
  switch (unzRet) {
    case UNZ_OK:
      strncpy(buf, "UNZ_OK", bufSize);
      break;
    case UNZ_END_OF_LIST_OF_FILE:
      strncpy(buf, "UNZ_END_OF_LIST_OF_FILE", bufSize);
      break;
    case UNZ_ERRNO:  // Alias of Z_ERRNO
      snprintf(buf, bufSize, "%d %s", errno, strerror(errno));
      break;
    case UNZ_PARAMERROR:
      strncpy(buf, "UNZ_PARAMERROR", bufSize);
      break;
    case UNZ_BADZIPFILE:
      strncpy(buf, "UNZ_BADZIPFILE", bufSize);
      break;
    case UNZ_INTERNALERROR:
      strncpy(buf, "UNZ_INTERNALERROR", bufSize);
      break;
    case UNZ_CRCERROR:
      strncpy(buf, "UNZ_CRCERROR", bufSize);
      break;
    case Z_STREAM_END:  // Not an error
      strncpy(buf, "Z_STREAM_END (not an error)", bufSize);
      break;
    case Z_NEED_DICT:  // Not an error
      strncpy(buf, "Z_NEED_DICT (not an error)", bufSize);
      break;
    case Z_STREAM_ERROR:
      strncpy(buf, "Z_STREAM_ERROR", bufSize);
      break;
    case Z_DATA_ERROR:
      strncpy(buf, "Z_DATA_ERROR", bufSize);
      break;
    case Z_MEM_ERROR:
      strncpy(buf, "Z_MEM_ERROR", bufSize);
      break;
    case Z_BUF_ERROR:
      strncpy(buf, "Z_BUF_ERROR", bufSize);
      break;
    case Z_VERSION_ERROR:
      strncpy(buf, "Z_VERSION_ERROR", bufSize);
      break;
    default:
      snprintf(buf, bufSize, "UNZ_invalid_%d", unzRet);
      break;
  }
}

static int readDownloadBytes(char* unzStr, size_t unzSize, unzFile unzf,
                             FILE* outf, char* syncStatus, size_t syncSize) {
  for (;;) {
    int r = unzReadCurrentFile(unzf, unzStr, unzSize);
    if (r == 0) {
      return 0;  // End of compressed file.
    }
    if (r < 0) {
      unzErrToString(r, unzStr, unzSize);
      if (snprintf(syncStatus, syncSize, "extract failed: %s\n", unzStr) < 0)
        ignore_truncated();
      return 1;
    }
    if (fwrite(unzStr, 1, r, outf) != (size_t)r) {
      if (snprintf(syncStatus, syncSize, "write failed: %d %s\n", errno,
                   strerror(errno)) < 0)
        ignore_truncated();
      return 1;
    }
  }
}

int RetroWeb::readDownload() {
  size_t syncSize = sizeof(syncStatus);
  unzFile unzf = unzOpen(syncOutfilename.c_str());
  if (!unzf) {
    if (snprintf(syncStatus, syncSize, "unable to read %s\n",
                 syncOutfilename.c_str()) < 0)
      ignore_truncated();
    return 1;
  }

  unz_global_info unzGlobal;
  char unzStr[4096];
  int r = unzGetGlobalInfo(unzf, &unzGlobal);
  if (r) {
    unzErrToString(r, unzStr, sizeof(unzStr));
    if (snprintf(syncStatus, syncSize, "unzGetGlobalInfo failed: %s\n",
                 unzStr) < 0)
      ignore_truncated();
    unzClose(unzf);
    return 1;
  }

  char found[sizeof(unzStr)];
  found[0] = 0;
  for (ZPOS64_T i = 0; i < unzGlobal.number_entry; i++) {
    if (i > 0) {
      r = unzGoToNextFile(unzf);
      if (r) {
        unzErrToString(r, unzStr, sizeof(unzStr));
        if (snprintf(syncStatus, syncSize, "unzGoToNextFile failed: %s\n",
                     unzStr) < 0)
          ignore_truncated();
        unzClose(unzf);
        return 1;
      }
    }

    unz_file_info unzFile;
    r = unzGetCurrentFileInfo(unzf, &unzFile, unzStr, sizeof(unzStr),
                              NULL /*extraField*/, 0, NULL /*szComment*/, 0);
    unzStr[sizeof(unzStr) - 1] = 0;

    // Check the file name in unzStr.
    // zipfiles directories are returned with a trailing '/' or '\\'.
    // There should be a single .so file, so check that found[0] is empty.
    char* ext = strrchr(unzStr, '.');
    if (!ext || strcmp(ext + 1, soExt.c_str()) || found[0]) {
      if (snprintf(syncStatus, syncSize, "unexpected contents: \"%s\"\n",
                   unzStr))
        ignore_truncated();
      if (found[0]) {
        // Wrote file successfully only to find more in the .zip later.
        unlink(found);
      }
      continue;
    }

    // The file name ends in soExt. Extract it.
    if (snprintf(found, sizeof(found), "%s%c%s", cachePath.c_str(), OS_SEP,
                 unzStr) < 0)
      ignore_truncated();
    r = unzOpenCurrentFile(unzf);
    if (r) {
      unzErrToString(r, unzStr, sizeof(unzStr));
      if (snprintf(syncStatus, syncSize,
                   "unzOpenCurrentFile failed on \"%s\": %s\n", found,
                   unzStr) < 0)
        ignore_truncated();
      unzClose(unzf);
      return 1;
    }

    FILE* outf = fopen(found, "wb");
    if (!outf) {
      if (snprintf(syncStatus, syncSize, "unable to create %s: %d %s\n", found,
                   errno, strerror(errno)) < 0)
        ignore_truncated();
      unzClose(unzf);
      return 1;
    }
    if (readDownloadBytes(unzStr, sizeof(unzStr), unzf, outf, syncStatus,
                          syncSize)) {
      char syncCopy[sizeof(syncStatus)];
      if (snprintf(syncCopy, sizeof(syncCopy), "\"%s\": %s", found,
                   syncStatus) < 0)
        ignore_truncated();
      strcpy(syncStatus, syncCopy);
      fclose(outf);
      unlink(found);
      unzClose(unzf);
      return 1;
    }
    fclose(outf);
  }

  r = unzClose(unzf);
  if (r) {
    unzErrToString(r, unzStr, sizeof(unzStr));
    if (snprintf(syncStatus, syncSize, "unzClose failed: %s\n", unzStr))
      ignore_truncated();
    return 1;
  }
  if (found[0]) {
    if (addToCores(found)) {
      return 1;
    }
  } else {
    snprintf(syncStatus, syncSize, "no .%s file found\n", soExt.c_str());
    return 1;
  }
  return 0;
}

static std::string getFullSavePath(const std::string& savePath,
                                   const std::string& name) {
  std::string r = savePath;
  r += OS_SEP;
  r += name;
  r += ".bin";
  return r;
}

int RetroWeb::loadSave(mapStringToApp::iterator curApp, std::string name,
                       std::string& appName) {
  appName.clear();
  std::string loadpath = getFullSavePath(getSavePath(), name);
  struct stat st;
  if (stat(loadpath.c_str(), &st)) {
    if (!ignoreFirstLoadStat) {
      logW("loadSave: stat(%s) failed: %d %s\n", loadpath.c_str(), errno,
           strerror(errno));
    }
    return 1;
  }
  if (st.st_mode & S_IFDIR) {
    logW("loadSave: not a file: %s\n", loadpath.c_str());
    return 1;
  }
  if ((size_t)st.st_size > protoSize(maxSaveDataLen)) {
    logW("loadSave: file %s too large: %lld\n", loadpath.c_str(),
         (long long)st.st_size);
    return 1;
  }
  std::vector<char> data;
  data.resize(st.st_size);
  FILE* fin = fopen(loadpath.c_str(), "rb");
  if (!fin) {
    logW("loadSave(%s): open failed: %d %s\n", loadpath.c_str(), errno,
         strerror(errno));
    return 1;
  }
  if (fread(data.data(), 1, st.st_size, fin) != (size_t)st.st_size) {
    logW("loadSave(%s): read failed: %d %s\n", loadpath.c_str(), errno,
         strerror(errno));
    return 1;
  }
  fclose(fin);

  auto* p = reinterpret_cast<const RetroSaveProto*>(data.data());
  // If core.getSupport() has p->gameType, use curApp. Otherwise find appName.
  if (curApp != getApps().end()) {
    if (curApp->second.getName() == p->gameName) {
      if (curApp->second.core.load(data)) {
        return 1;
      }
      return 0;
    }
  }
  // This save needs a different app.
  bool foundAppNameMatch = false;
  for (auto i = getApps().begin(); i != getApps().end(); i++) {
    if (i->second.getName() == p->gameName) {
      foundAppNameMatch = true;
      auto& oneApp = i->second;
      if (!p->gameType[0]) {
        // If p->gameType is empty, only p->gameName matters.
        appName = p->gameName;
        return 0;
      }
      for (auto& oneExt : oneApp.core.getSupport()) {
        if (oneExt == p->gameType) {
          // oneApp matches both p->gameName and p->gameType. Set appName.
          appName = p->gameName;
          return 0;
        }
      }
      auto r = typeMap.find(p->gameType);
      if (r == typeMap.end()) {
        // There is no core that supports this gameType.
        logW("loadSave(%s): \"%s\" %s \"%s\"?\n", loadpath.c_str(), p->gameName,
             "has the right name but does not support", p->gameType);
        return 1;
      }
      if (r->second.empty()) {
        logW("loadSave(%s): BUG: type %s with no cores?\n", loadpath.c_str(),
             p->gameType);
        return 1;
      }
      // There are cores that support this gameType.
      auto c = cores.find(r->second.at(0));
      if (c == cores.end()) {
        logW("loadSave(%s): BUG: cannot find a core \"%s\"\n", loadpath.c_str(),
             r->second.at(0).c_str());
      }
      logW("loadSave(%s): \"%s\" %s \"%s\". Look for \"%s\" roms?\n",
           loadpath.c_str(), p->gameName,
           "has the right name but does not support", p->gameType,
           c->second.getName().c_str());
    }
  }
  if (!foundAppNameMatch) {
    logW("loadSave(%s): \"%s\" can load this, but cannot be found.\n",
         loadpath.c_str(), p->gameName);
    std::string where = getGamePath();
#ifdef __ANDROID__
    where = "/sdcard (and give app permission for external storage)";
#endif /*__ANDROID__*/
    logW("loadSave(%s): find it online? Add it to %s\n", loadpath.c_str(),
         where.c_str());
  }
  return 1;
}

int RetroWeb::saveTo(RetroApp& app, std::string name) {
  std::vector<char> data;
  if (app.core.save(data)) {
    return 1;
  }
  auto* p = reinterpret_cast<RetroSaveProto*>(data.data());
  strncpy(p->gameName, app.getName().c_str(), sizeof(p->gameName) - 1);
  strncpy(p->gameType, app.getType().c_str(), sizeof(p->gameType) - 1);

  std::string outpath = getFullSavePath(getSavePath(), name);
  FILE* outf = fopen(outpath.c_str(), "wb");
  if (!outf) {
    logE("saveTo(%s) failed: %d %s\n", outpath.c_str(), errno, strerror(errno));
    return 1;
  }
  if (fwrite(p, 1, data.size(), outf) != data.size()) {
    logE("saveTo(%s) write failed: %d %s\n", outpath.c_str(), errno,
         strerror(errno));
    fclose(outf);
    unlink(outpath.c_str());
    return 1;
  }
  fclose(outf);
  saves.emplace(name);
  return 0;
}

int RetroWeb::renameSave(std::string from, std::string to) {
  auto p = saves.find(from);
  if (p == saves.end()) {
    logE("renameSave: from=\"%s\" not found\n", from.c_str());
    return 1;
  }
  auto q = saves.find(to);
  if (q != saves.end()) {
    logE("renameSave: to=\"%s\" already exists\n", to.c_str());
    return 1;
  }
  std::string frompath = getFullSavePath(getSavePath(), from);
  std::string topath = getFullSavePath(getSavePath(), to);
  if (::rename(frompath.c_str(), topath.c_str())) {
    logE("renameSave: %d %s\n", errno, strerror(errno));
    return 1;
  }
  saves.erase(p);
  saves.emplace(to);
  return 0;
}

const std::string& RetroCore::getCachePath() const {
  return parent.getCachePath();
}

const std::string& RetroCore::getSystemDir() const {
  return parent.getSystemDir();
}

const std::string& RetroCore::getSavePath() const {
  return parent.getSavePath();
}
