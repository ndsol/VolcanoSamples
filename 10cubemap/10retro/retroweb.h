/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This accesses http://buildbot.libretro.com
 */

#pragma once

#include "retrocore.h"

// dataCallbackFn receives the data from RetroHttp. See setDataCallback.
typedef int (*dataCallbackFn)(void* self, void* data, size_t len,
                              size_t maxlen);

// RetroHttp manages HTTP requests.
// (only 1 request at a time keeps this a simple API.)
typedef struct RetroHttp {
  virtual ~RetroHttp();

  enum {
    FAIL_IN_INIT = -4,
    FAIL_VIA_WORKER_BAD_DATA = -3,
    FAIL_VIA_HTTP_STATUS = -2,
    FAIL_DATA_CALLBACK = -1,
    RETRO_NO_ERROR = 0
  };

  // ctorError must be called before any other methods are called.
  // returns 0 = success, nonzero = failure.
  int ctorError();

  // clearError resets getErrno to NO_ERROR.
  void clearError();

  // setDataCallback must be called before startGET. The callback pointer is
  // copied to the internal data structure in startGET. The callback is called
  // repeatedly to write data. Call pollGET until it returns 0 to end.
  //
  // The callback receives 'len' bytes in 'data' (not null terminated).
  // The callback also receives a 'maxlen' hint, which starts as 0 (length
  // unknown) but on further callbacks can be non-zero as a hint to avoid
  // unnecessary reallocation of arrays.
  //
  // If the callback returns non-zero, the download is aborted and getErrno()
  // returns FAIL_DATA_CALLBACK.
  void setDataCallback(void* self, dataCallbackFn cb) {
    dataCbSelf = self;
    dataCb = cb;
  }

  int startGET(const char* url);
  void removeGET();

  // returns 0=failed, completed, or never started. progress is set to 0.
  //         1=request in progress. progress is set to a fraction 0..1.
  int pollGET(float& progress);

  // getErrno returns one of the enum constants above, such as NO_ERROR,
  // or a curl error code.
  int getErrno() const;

  // getError returns a string description of the status.
  const char* getError() const;

  // getStatusCode returns an HTTP status code, or 0 if none is available.
  long getStatusCode() const;

  // getUrlPath returns the CURLUPART_PATH after a startGET, or nullptr if err
  const char* getUrlPath();

  // reset stops any current operation and resets everything. You must call
  // ctorError again before any other operation.
  void reset();

  int onGlfwIO(int fd, int eventBits);
  int poll();

 protected:
  void* internal{nullptr};
  void* dataCbSelf{nullptr};
  dataCallbackFn dataCb{nullptr};
} RetroHttp;

typedef std::map<std::string, RetroApp> mapStringToApp;

typedef struct RetroWeb {
  virtual ~RetroWeb();

  RetroHttp http;

  // ctorError must be called before any other methods are called.
  // returns 0 = success, nonzero = failure.
  int ctorError();

  // stop stops any sync in progress.
  int stop();

  // poll should be called every time through the main loop. It will return
  // immediately if no operation is underway.
  //
  // returns 0 = success, nonzero = failure.
  int poll();

  bool isSyncRunning() const { return !!syncRunning; }

  // startSync connects to the RetroArch website and syncs any libretro
  // cores, indexes, etc.
  // NOTE: A failure to sync is not reported as an error. Check the sync status
  //       with getLastSyncError().
  // returns 0 = success, nonzero = failure.
  int startSync();

  // getLastSyncError returns the error code of the last attempt to sync.
  // It can be one of CURLE_* defined by libcurl or FAIL_VIA_HTTP_STATUS or
  // another value from that enum.
  int getLastSyncError() const;

  // getSyncStatus returns a human-readable progress message.
  const char* getSyncStatus();

  // getCachePath returns the location of synced core files.
  const std::string& getCachePath() const { return cachePath; }

  // getSystemDir returns the location of synced system info files.
  const std::string& getSystemDir() const { return systemDir; }

  // getSavePath returns the location of save files.
  const std::string& getSavePath() const { return savePath; }

  const std::string& getGamePath() const { return gamePath; }

  size_t getCoresLen() const { return cores.size(); }

  mapStringToApp& getApps() { return apps; }

  std::set<std::string> saves;

  // ignoreFirstLoadStat can be set to true if loadSave() will be called from
  // an automated process (such as restoring an auto-saved state). If there is
  // an error, nothing will be logged (but loadSave still returns 1).
  bool ignoreFirstLoadStat{false};

  // loadSave will attempt to load the save into curApp. If it belongs to a
  // different app, loadSave will *not* load it, but appName will be non-empty
  // after it returns. Load that other app (appName is in getApps()), then
  // try again.
  int loadSave(mapStringToApp::iterator curApp, std::string name,
               std::string& appName);

  // saveTo will save the current state.
  int saveTo(RetroApp& app, std::string name);

  // renameSave will rename the file and update saves.
  int renameSave(std::string from, std::string to);

  int listCacheContents();

 protected:
  char syncStatus[256]{0};
  char shortFilename[256]{0};
  FILE* syncOutf{NULL};
  float syncProgress{0.f};
  int syncGetRunning{0};
  int syncRunning{0};
  std::string syncTmpdir;
  std::string syncOutfilename;
  std::string cachePath;
  std::string systemDir;
  std::string savePath;
  std::string gamePath;
  std::string buildbotUrl;       // Base URL for libretro cores.
  std::string buildbotPlatform;  // Path in buildbotUrl for this platform.
  std::string buildbotArch;      // Path in buildbotPlatform for this CPU arch.
  std::string soExt;  // Platform-specific extension for dll/so/dylib.
  std::map<std::string, RetroCore> cores;
  std::map<std::string, std::vector<std::string>> typeMap;
  mapStringToApp apps;
  bool haveNstDatabase{false};

  std::string getUrlFor(std::string filename) const;
  std::string getFileType(const std::string filename, off_t filesize);
  void cleanup();
  int createTmpdir();
  int readDownload();
  static int dataCallback(void* self, void* data, size_t len, size_t maxlen);

  int initCache();
  int checkPlatform();
  int addToCores(const char* pathname);
} RetroWeb;

#ifdef __ANDROID__
extern "C" void androidSetNeedsMenuKey(int menuKey);
#endif
