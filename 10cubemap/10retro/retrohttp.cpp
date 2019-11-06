/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * RetroHttp implementation
 */

#include <curl/urlapi.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "retroweb.h"
#include "../../src/base_application.h"

#ifdef _WIN32
#include <io.h>  // for _open_osfhandle
#include <fcntl.h>  // for _O_RDWR

extern "C" {

int Curl_socket_check(curl_socket_t readfd, curl_socket_t readfd2,
                      curl_socket_t writefd, time_t timeout_ms);

// Curl_win32_swrite is implemented below.
int Curl_win32_swrite(curl_socket_t s, void* buf, size_t len);
// Curl_win32_sread is implemented below.
int Curl_win32_sread(curl_socket_t s, void* buf, size_t len);

}  // extern "C"
#endif  /*_WIN32*/

struct RetroInternal;

// libuv + glfw is still in "proposed" status after many years:
// https://github.com/libuv/leps/pull/3
// https://github.com/joyent/libuv/issues/1246#issuecomment-41254223
// "In hindsight, we should've made libuv default to pull instead of push
// (one of my few major regrets)"
//
// GLFW alone is now sufficient.
typedef struct RetroSocket {
  RetroSocket(RetroInternal& parent, CURL* curlHandle, CURLU* curlurl)
      : parent{parent}, curlHandle{curlHandle}, curlurl{curlurl} {}

  RetroInternal& parent;
  CURL* curlHandle;
  CURLU* curlurl{nullptr};
  char* urlPath{nullptr};
  long long getTotal{-1}, getNow{0}, putTotal{-1}, putNow{0};
  long httpCode{0};
  std::string startedURL;
  void* dataCbSelf{nullptr};
  dataCallbackFn dataCb{nullptr};
  int errState{RetroHttp::RETRO_NO_ERROR};
  char curlErr[CURL_ERROR_SIZE];
  int done{0};

  int eventBits{0};
  int fd{-1};

  const char* getUrlPath() {
    if (urlPath) {
      curl_free(urlPath);
    }
    urlPath = nullptr;
    CURLUcode r = curl_url_get(curlurl, CURLUPART_PATH, &urlPath, CURLU_DEFAULT_SCHEME);
    if (r) {
      logE("RetroHttp::getUrlPath: curl_url_get failed: %d\n", r);
      return nullptr;
    }
    return urlPath;
  }

  size_t curlHeader(char* header, size_t len);

  static size_t curlHeaderCb(char* header, size_t len, size_t n, void* self) {
    return static_cast<RetroSocket*>(self)->curlHeader(header, len * n);
  }

  int curlXfer(curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
               curl_off_t ulnow) {
    putNow = ulnow;
    if (ultotal > 0) {
      putTotal = ultotal;
    }
    getNow = dlnow;
    if (dltotal > 0) {
      getTotal = dltotal;
    }
    return 0;
  }

  static int curlXferCb(void* self, curl_off_t dltotal, curl_off_t dlnow,
                        curl_off_t ultotal, curl_off_t ulnow) {
    return static_cast<RetroSocket*>(self)->curlXfer(dltotal, dlnow, ultotal,
                                                     ulnow);
  }

  size_t curlWrite(void* data, size_t len);

  static size_t curlWriteCb(void* data, size_t len, size_t n, void* self) {
    return static_cast<RetroSocket*>(self)->curlWrite(data, len * n);
  }

  int finish(CURLcode code) {
    done = 1;
    if (code == CURLE_WRITE_ERROR &&
        (errState == RetroHttp::FAIL_VIA_HTTP_STATUS ||
         errState == RetroHttp::FAIL_DATA_CALLBACK)) {
      logW("failed to sync %s\n", startedURL.c_str());
    } else if (errState != RetroHttp::RETRO_NO_ERROR) {
      logW("failed to sync %s\n", startedURL.c_str());
      logW("curl_easy_perform: %d %s\n", (int)code, curl_easy_strerror(code));
      logW("curl error \"%s\"\n", curlErr);
    }
    return 0;
  }

  void reset(CURLM* curlm) {
#ifdef _WIN32
    sovl = NULL;
    if (fd != -1) {
      // _close closes the socket using CloseHandle(), which used to leak
      // some memory: https://stackoverflow.com/questions/4676256
      _close(fd);
      fd = -1;
    }
#else  /*_WIN32*/
    fd = -1;  // Since on posix fd was not opened here, do not close it here.
#endif  /*_WIN32*/

    if (urlPath) {
      curl_free(urlPath);
      urlPath = nullptr;
    }

    if (curlurl) {
      curl_url_cleanup(curlurl);
      curlurl = nullptr;
    }

    if (curlHandle) {
      curl_multi_remove_handle(curlm, curlHandle);
      curl_easy_cleanup(curlHandle);
      curlHandle = nullptr;
    }
  }

  int curlUpdateSocket(curl_socket_t s, int what) {
    int bits = 0;

    switch (what) {
    case CURL_POLL_REMOVE:
      // NOTE: events may still be reported after deleting the FD.
      if (!glfwEventDelFD(fd, eventBits)) {
        logE("curlUpdateSocket: CURL_POLL_REMOVE %d but glfwEventDelFD failed\n",
             fd);
        return 0;
      }
      return 0;

    // These other 'what' cases are all to add a socket curl is telling us about.
    case CURL_POLL_IN:
      bits |= GLFW_IO_READ;
      break;
    case CURL_POLL_OUT:
      bits |= GLFW_IO_WRITE;
      break;
    case CURL_POLL_INOUT:
      bits |= GLFW_IO_READ | GLFW_IO_WRITE;
      break;
    default:
      logE("curlUpdateSocket: what=%d unsupported\n", what);
      return -1;
    }

    int prevFd = fd;
#ifdef _WIN32
    eventBits = GLFW_IO_READ | GLFW_IO_WRITE | GLFW_IO_RDHUP | GLFW_IO_HUP | GLFW_IO_ERR;
    if (bits & GLFW_IO_READ) {
      enableRecv = 1;
      // doCurlRead() will call WSARecv()
      canRecv = 1;
    }
    if (bits & GLFW_IO_WRITE) {
      // Notify libcurl immediately that write can be performed.
      // When it calls Curl_win32_swrite() -> doCurlWrite() then send the bytes to WSASend().
      canSend = 1;
    }

    // If this is the first time s has been seen, convert a Win SOCKET to an int fd.
    if (actual == NULL) {
      actual = s;
      WSAPROTOCOL_INFOA info;
      if (WSADuplicateSocketA(s, GetCurrentProcessId(), &info)) {
        logE("WSADuplicateSocketA failed: %x\n", WSAGetLastError());
        return 0;
      }
      sovl = WSASocket(info.iAddressFamily, info.iSocketType, info.iProtocol,
                       &info, 0 /*group*/, WSA_FLAG_OVERLAPPED);
      if (sovl == INVALID_SOCKET) {
        logE("WSASocket(dup) failed: %x\n", WSAGetLastError());
        return 0;
      }
      fd = _open_osfhandle((intptr_t)sovl, _O_RDWR);
      if (fd < 0) {
        logE("_ofs_openhandle failed: %d\n", errState);
        return 0;
      }
    }
#else  /*_WIN32*/
    eventBits |= bits | GLFW_IO_RDHUP | GLFW_IO_HUP | GLFW_IO_ERR;
    fd = s;
#endif  /*_WIN32*/
    if (prevFd == -1) {
      if (!glfwEventAddFD(fd, eventBits)) {
        logE("curlDoSocket: glfwEventAddFD(%d, %x) failed\n", fd, eventBits);
        return 0;
      }
    } else {
      if (!glfwEventModifyFD(fd, eventBits)) {
        logE("curlDoSocket: glfwEventModifyFD(%d, %x) failed\n", fd, eventBits);
        return 0;
      }
    }
    return 0;
  }

  static int curlLogCb(CURL* easy, curl_infotype type, char* data, size_t size, void* userptr) {
    auto* sock = static_cast<RetroSocket*>(userptr);
    (void)sock;
    (void)easy;
    switch (type) {
    case CURLINFO_TEXT:
      logI("curl: %.*s%s", (int)size, data, data[size - 1] == '\n' ? "" : "\n");
      break;
    case CURLINFO_HEADER_IN:
      logI("curl HDR %zu in\n", size);
      break;
    case CURLINFO_HEADER_OUT:
      logI("curl HDR %zu out\n", size);
      break;
    case CURLINFO_DATA_IN:
      logI("curl DATA %zu in\n", size);
      break;
    case CURLINFO_DATA_OUT:
      logI("curl DATA %zu out\n", size);
      break;
    case CURLINFO_SSL_DATA_OUT:
      logI("curl SSL %zu out\n", size);
      break;
    case CURLINFO_SSL_DATA_IN:
      logI("curl SSL %zu in\n", size);
      break;
    default:
      logW("curl: log type=%d unknown\n", (int)type);
      break;
    }
    return 0;
  }

#ifdef _WIN32
  curl_socket_t actual{NULL};
  SOCKET sovl;

  // recvbuf allows the IOCP to complete before libcurl asks for the data
  char recvbuf[4096];
  // recvInBuf tracks how many bytes are held from the IOCP before doCurlRead()
  DWORD recvInBuf{0};
  // recvInKernel:
  // libcurl depends on select(), and will only read from a socket if the kernel still holds
  // bytes for it. Thus this code only calls WSARecv() when it is expected the kernel will be
  // able to complete the WSARecv() without using an IOCP and fill the buffer with data immediately.
  DWORD recvInKernel{0};

  // sendbuf, sendindir keep track of the IOCP after libcurl hands off the data
  char sendbuf[4096];
  WSABUF sendindir;

  OVERLAPPED ovRecv;
  OVERLAPPED ovSend;

  // enableRecv, if not set, prevents any read operation on the socket
  // Sometimes libcurl will call doCurlRecv() without having first asked
  // for CURL_POLL_IN. In that case, tell libcurl WSAEWOULDBLOCK (which
  // libcurl treats like a posix EAGAIN). libcurl then realizes it needs
  // CURL_POLL_IN and retries.
  //
  // There is no "enableSend" because the Windows IOCP calls do not include
  // such things as a GLFW_IO_WRITE bit before async sending is allowed.
  // This class can just call WSASend() and later get an IOCP callback when
  // the send operation completed.
  DWORD enableRecv{0};
  DWORD canRecv{0};

  // canSend is used to keep track of whether a WSASend is in flight. If
  // nothing is in flight, canSend = 1.
  DWORD canSend{0};

  // pump() is called whenever there is any movement on the socket. libcurl uses
  // select() and will silently ignore a socket if it can't read from it, so
  // pump() is to resume the forward progress once recvInKernel is set.
  int pump(CURLM* curlm) {
    int bits = 0;
    if (canSend) {
      bits |= CURL_CSELECT_OUT;
    }
    if (enableRecv) {
      // Will libcurl want to read something?
      // Call the same API that libcurl uses to see if recvInKernel should be set.
      int r = Curl_socket_check(actual, CURL_SOCKET_BAD, CURL_SOCKET_BAD, 0);
      if (r < 0) {
        logE("Curl_socket_check failed: %x\n", WSAGetLastError());
      } else if (r > 0) {
        recvInKernel = 1;
      }
      if (recvInBuf || recvInKernel) {
        bits |= CURL_CSELECT_IN;
      }
    }
    if (bits) {
      int count{0};
      CURLMcode res = curl_multi_socket_action(curlm, actual, bits, &count);
      if (res != CURLM_OK) {
        logE("pump: curl_multi_socket_action(%p, %d) for %d bytes: %d %s",
             (void*)actual, bits, (int)recvInBuf, res, curl_multi_strerror(res));
        return 1;
      }
    }
    return 0;
  }

  int actuallyWSARecv() {
    memset(&ovRecv, 0, sizeof(ovRecv));
    WSABUF recvindir;
    recvindir.len = sizeof(recvbuf);
    recvindir.buf = recvbuf;
    DWORD xfer{0};
    DWORD flags{0};
    int r = WSARecv(sovl, &recvindir, 1, &xfer, &flags, &ovRecv, NULL);
    if (r != 0) {
      r = WSAGetLastError();
      if (r != WSA_IO_PENDING) {
        logE("WSARecv failed: %d\n", r);
        return -1;
      }
      canRecv = 0;
      return -1;
    }

    // The recv data is here.
    /*r = WSAGetLastError();
    logI("WSARecv: recvInBuf=%d - canRev = 1: WSAGetLastError = %d  internal = %d\n", (int)recvInBuf, r, (int)ovRecv.Internal);*/
    return 0;
  }

  int recvGetOverlappedResult() {
    DWORD ovLen;
    if (!GetOverlappedResult((HANDLE)sovl, &ovRecv, &ovLen, FALSE)) {
      DWORD e = GetLastError();
      if (e != ERROR_HANDLE_EOF) {
        logE("recv GetOverlappedResult failed: %x\n", e);
        WSASetLastError(e);
        return 1;
      }
      recvInBuf = 0;
      // Do not notify curl for ERROR_HANDLE_EOF.
    } else if (ovRecv.Internal != STATUS_PENDING) {
      if (ovRecv.Internal != ERROR_SUCCESS) {
        logE("doCurlRead(%zu): recv status = %x\n", ovRecv.Internal);
        WSASetLastError(ovRecv.Internal);
        return 1;
      }
      recvInBuf = ovLen;
      canRecv = 1;
    }
    return 0;
  }

  int doCurlRead(void* buf, size_t len) {
    recvInKernel = 0;

    // Fetch bytes out of the kernel
    while (!recvInBuf) {
      if (enableRecv) {
        int r = canRecv ? actuallyWSARecv()
                        : 0;  // canRecv == 0: Do not call WSARecv. Just poll ovRecv with GetOverlappedResult().

        if (r == 0) {
          if (recvGetOverlappedResult()) {
            logE("doCurlRead(%zu): recvGetOverlappedResult\n", len);
            return -1;
          }
          if (recvInBuf) {
            break;  // recvGetOverlappedResult can update recvInBuf with bytes received.
          }
        }
      }

      // Tell libcurl using WASGetLastError() to call doCurlRead again later.
      WSASetLastError(WSAEWOULDBLOCK);
      return -1;
    }
    DWORD r = recvInBuf;
    if (r > len) {
      r = len;
    }
    recvInBuf -= r;
    memcpy(buf, recvbuf, r);
    memmove(&recvbuf[0], &recvbuf[r], recvInBuf);
    return r;
  }

  int doCurlWrite(void* buf, size_t len) {
    if (fd == -1) {
      WSASetLastError(WSAENOTSOCK);
      return -1;
    }
    if (len > sizeof(sendbuf)) {
      len = sizeof(sendbuf);
    }
    memcpy(sendbuf, buf, len);
    memset(&ovSend, 0, sizeof(ovSend));
    sendindir.len = len;
    sendindir.buf = sendbuf;
    DWORD xfer{0};
    int r = WSASend(sovl, &sendindir, 1, &xfer, 0, &ovSend, NULL);
    if (r != 0) {
      r = WSAGetLastError();
      if (r != WSA_IO_PENDING) {
        logE("WSASend failed: %d\n", r);
        return -1;
      }
    }
    canSend = 0;
    return len;
  }
#endif  /*_WIN32*/
} RetroSocket;

typedef struct RetroInternal {
  int errState{RetroHttp::RETRO_NO_ERROR};
  std::map<CURL*, std::shared_ptr<RetroSocket>> map;

  CURLM* curlm{nullptr};
  Timer timer;
  double nextTimer{-1.};

  int onGlfwIOsock(RetroSocket& sock, int eventBits) {
#ifdef _WIN32
    if (sock.pump(curlm)) {
      logE("onGlfwIOSock(%p): pump failed\n", (void*)sock.actual);
      return 1;
    }

    // Do not call GetOverlappedResult if canSend == 1: canSend == 1 means there is no send op in flight
    if (sock.canSend == 0) {
      DWORD ovLen;
      if (!GetOverlappedResult((HANDLE)sock.sovl, &sock.ovSend, &ovLen, FALSE)) {
        DWORD e = GetLastError();
        logE("onGlfwIO: send GetOverlappedResult failed: %x\n", e);
        return 1;
      } else if (sock.ovSend.Internal != STATUS_PENDING) {
        if (sock.ovSend.Internal != ERROR_SUCCESS) {
          logE("onGlfwIO: send failed = %x\n", sock.ovSend.Internal);
          return 1;
        }
        if (ovLen != sock.sendindir.len) {
          logE("onGlfwIO: send only did %d want %d\n", (int)ovLen, sock.sendindir.len);
          return 1;
        }
        sock.sendindir.len = 0;
        sock.canSend = 1;
      }
    }
#else  /*_WIN32*/
    int bits = ((eventBits & GLFW_IO_READ) ? CURL_CSELECT_IN : 0) |
               ((eventBits & GLFW_IO_WRITE) ? CURL_CSELECT_OUT : 0);
    int count{0};
    CURLMcode res = curl_multi_socket_action(curlm, sock.fd, bits, &count);
    if (res != CURLM_OK) {
      logE("onGlfwIOsock: curl_multi_socket_action(%d, %d): %d %s",
           sock.fd, bits, res, curl_multi_strerror(res));
      return 1;
    }
#endif  /*_WIN32*/
    return 0;
  }

  static int curlSocketCb(CURL *easy, curl_socket_t s, int what, void *userp, void *socketp) {
    (void)socketp;  // curlSocketCb is first chance to set socketp! Ignore it.
    auto* ri = static_cast<RetroInternal*>(userp);
    auto mapi = ri->map.find(easy);
    if (mapi == ri->map.end()) {
      logE("curlSocketCb: what = %d on unknown easy = %p\n", what, easy);
      return 0;
    }
    if (mapi->second->curlUpdateSocket(s, what)) {
      logE("curlSocketCb: curlUpdateSocket failed\n");
      return -1;
    }
    return 0;
  }

  static int curlTimerCb(CURLM *multi, long timeout_ms, void *userp) {
    (void)multi;
    auto* ri = static_cast<RetroInternal*>(userp);
    if (timeout_ms < 0) {
      ri->nextTimer = -1.;
    } else {
      ri->nextTimer = ri->timer.now() + (timeout_ms * 1e-3);
    }
    return 0;
  }
} RetroInternal;

int RetroHttp::onGlfwIO(int fd, int eventBits) {
  auto* ri = static_cast<RetroInternal*>(internal);
  if (!ri) {
    logE("onGlfwIO(%d, %d): internal == NULL\n", fd, eventBits);
    return GLFW_TRUE;
  }
  for (auto mapi = ri->map.begin(); mapi != ri->map.end(); mapi++) {
    if (mapi->second && mapi->second->fd == fd) {
      return ri->onGlfwIOsock(*mapi->second, eventBits) ? GLFW_FALSE : GLFW_TRUE;
    }
  }
  logE("onGlfwIO(%d, %d): no socket for that fd\n", fd, eventBits);
  return GLFW_TRUE;
}

int RetroHttp::poll() {
  auto* ri = static_cast<RetroInternal*>(internal);
  if (!ri) {
    return 0;
  }

  for (;;) {
    int count{0};
    CURLMsg* m = curl_multi_info_read(ri->curlm, &count);
    if (!m) {
      break;
    }
    if (m->msg != CURLMSG_DONE) {
      logE("curl_multi_info_read: want CURLMSG_DONE (%d) got %d\n", CURLMSG_DONE, m->msg);
      return 1;
    }
    auto mapi = ri->map.find(m->easy_handle);
    if (mapi == ri->map.end()) {
      logE("curl_multi_info_read: no existing easyHandle %p\n", m->easy_handle);
      return 1;
    }
    if (mapi->second->finish(m->data.result)) {
      logE("RetroSocket finish failed\n");
      return 1;
    }
  }
#ifdef _WIN32
  if (!ri->map.empty()) {
    for (auto mapi = ri->map.begin(); mapi != ri->map.end(); mapi++) {
      if (mapi->second && mapi->second->pump(ri->curlm)) {
        return 1;
      }
    }
  }
#endif  /*_WIN32*/

  if (ri->nextTimer < 0.) {
    return 0;
  }
  double n = ri->timer.now();
  if (n < ri->nextTimer) {
    return 0;
  }
  ri->nextTimer = -1.;
  int count{0};
  CURLMcode res = curl_multi_socket_action(ri->curlm, CURL_SOCKET_TIMEOUT, 0, &count);
  if (res != CURLM_OK) {
    logE("curl_multi_socket_action(CURL_SOCKET_TIMEOUT): %d %s\n", res, curl_multi_strerror(res));
    return 1;
  }
  return 0;
}

int RetroHttp::pollGET(float& progress) {
  auto* ri = static_cast<RetroInternal*>(internal);
  if (!ri) {
    logE("pollGET: must call ctorError first\n");
    return 1;
  }

  progress = 0.f;
  if (ri->errState) {
    return 0;  // GET failed. 0 means GET is done.
  }
  if (ri->map.empty()) {
    return 0;  // There is nothing running.
  }
  auto& sock = *ri->map.begin()->second;
  if (sock.errState || sock.done) {
    return 0;  // GET either failed or succeeded. 0 means GET is done.
  }
  if (sock.getTotal <= 0) {
    progress = 0.f;
  } else {
    progress = ((float)sock.getNow) / sock.getTotal;
  }
  return 1;
}

#ifdef _WIN32
static RetroInternal* _global_ri{ nullptr };

int Curl_win32_swrite(curl_socket_t s, void* buf, size_t len) {
  if (!_global_ri) {
    logE("Curl_win32_swrite: _global_ri = NULL\n");
    return -1;
  }
  for (auto mapi = _global_ri->map.begin(); mapi != _global_ri->map.end(); mapi++) {
    if (mapi->second->actual == s) {
      return mapi->second->doCurlWrite(buf, len);
    }
  }
  //logE("Curl_win32_swrite: no socket found for %p\n", (void*)s);
  return -1;
}

int Curl_win32_sread(curl_socket_t s, void* buf, size_t len) {
  if (!_global_ri) {
    logE("Curl_win32_sread: _global_ri = NULL\n");
    return -1;
  }
  for (auto mapi = _global_ri->map.begin(); mapi != _global_ri->map.end(); mapi++) {
    if (mapi->second->actual == s) {
      return mapi->second->doCurlRead(buf, len);
    }
  }
  //logE("Curl_win32_sread: no socket found for %p\n", (void*)s);
  return -1;
}
#endif  /*_WIN32*/

RetroHttp::~RetroHttp() {
  reset();
}

int RetroHttp::ctorError() {
  RetroInternal* ri;
  if (internal) {
    ri = static_cast<RetroInternal*>(internal);
  } else {
    ri = new RetroInternal;
    internal = static_cast<void*>(ri);
  }
#ifdef _WIN32
  _global_ri = ri;
#endif  /*_WIN32*/
  ri->errState = RetroHttp::FAIL_IN_INIT;
  if (!ri->map.empty()) {
    logE("RetroWeb: ctorError called twice - call reset() first\n");
    return 1;
  }

  ri->curlm = curl_multi_init();
  if (!ri->curlm) {
    logE("RetroWeb: curl_multi_init failed\n");
    return 1;
  }
  if (curl_multi_setopt(ri->curlm, CURLMOPT_SOCKETDATA, ri) ||
    curl_multi_setopt(ri->curlm, CURLMOPT_SOCKETFUNCTION, RetroInternal::curlSocketCb) ||
    curl_multi_setopt(ri->curlm, CURLMOPT_TIMERDATA, ri) ||
    curl_multi_setopt(ri->curlm, CURLMOPT_TIMERFUNCTION, RetroInternal::curlTimerCb)) {
    logE("RetroWeb: curl_multi_setopt failed\n");
    return 1;
  }
  ri->errState = RetroHttp::RETRO_NO_ERROR;
  return 0;
}

void RetroHttp::clearError() {
  auto* ri = static_cast<RetroInternal*>(internal);
  if (!ri) {
    logE("clearError: must call ctorError first\n");
    return;
  }
  ri->errState = RetroHttp::RETRO_NO_ERROR;
}

int RetroHttp::startGET(const char* url) {
  auto* ri = static_cast<RetroInternal*>(internal);
  if (!ri) {
    logE("startGET: must call ctorError first\n");
    return 1;
  }
  if (ri->errState) {
    logE("startGET: must call clearError first\n");
    return 1;
  }

  auto easyHandle = curl_easy_init();
  if (!easyHandle) {
    logE("RetroWeb: curl_easy_init failed\n");
    return 1;
  }
  auto curlurl = curl_url();
  if (!curlurl) {
    logE("RetroWeb: curl_url failed\n");
    return 1;
  }

  auto easyi = ri->map.emplace(easyHandle, std::make_shared<RetroSocket>(*ri, easyHandle, curlurl));
  if (!easyi.second) {
    logE("RetroWeb: map.emplace: easyHandle %p already in map\n", easyHandle);
    return 1;
  }
  RetroSocket& sock = *easyi.first->second;
  ri->errState = RetroHttp::FAIL_IN_INIT;

  if (curl_easy_setopt(easyHandle, CURLOPT_FOLLOWLOCATION, 1L) ||
    curl_easy_setopt(easyHandle, CURLOPT_DEBUGFUNCTION, RetroSocket::curlLogCb) ||
    curl_easy_setopt(easyHandle, CURLOPT_DEBUGDATA, (void*)&sock) ||
    curl_easy_setopt(easyHandle, CURLOPT_USERAGENT, "libretro") ||
    curl_easy_setopt(easyHandle, CURLOPT_HEADERFUNCTION, RetroSocket::curlHeaderCb) ||
    curl_easy_setopt(easyHandle, CURLOPT_XFERINFOFUNCTION, RetroSocket::curlXferCb) ||
    curl_easy_setopt(easyHandle, CURLOPT_WRITEFUNCTION, RetroSocket::curlWriteCb) ||
    curl_easy_setopt(easyHandle, CURLOPT_HEADERDATA, (void*)&sock) ||
    curl_easy_setopt(easyHandle, CURLOPT_XFERINFODATA, (void*)&sock) ||
    curl_easy_setopt(easyHandle, CURLOPT_WRITEDATA, (void*)&sock) ||
    curl_easy_setopt(easyHandle, CURLOPT_ERRORBUFFER, (void*)sock.curlErr) ||
    curl_easy_setopt(easyHandle, CURLOPT_NOPROGRESS, 0L)) {
    logE("RetroWeb: curl_easy_setopt failed\n");
    return 1;
  }
  CURLUcode r = curl_url_set(sock.curlurl, CURLUPART_URL, url, CURLU_DEFAULT_SCHEME | CURLU_URLENCODE);
  if (r) {
    logE("startGET(%s): curl_url_set %d\n", url, r);
    return 1;
  }
  if (curl_easy_setopt(easyHandle, CURLOPT_CURLU, sock.curlurl)) {
    logE("worker: curl_easy_setopt(CURLOPT_CURLU) failed\n");
    return 1;
  }
  sock.dataCb = dataCb;
  sock.dataCbSelf = dataCbSelf;
  sock.startedURL = url;
  sock.curlErr[0] = 0;

  // Adding easyHandle to ri->curlm is the curl_multi_* equivalent of curl_easy_perform().
  // This starts the GET request.
  CURLMcode res = curl_multi_add_handle(ri->curlm, easyHandle);
  if (res != CURLM_OK) {
    logE("curl_multi_add_handle(%s) failed: %d %s\n", url, res, curl_multi_strerror(res));
    return 1;
  }
  ri->errState = RetroHttp::RETRO_NO_ERROR;
  return 0;
}

int RetroHttp::getErrno() const {
  auto* ri = static_cast<RetroInternal*>(internal);
  if (!ri) {
    return FAIL_IN_INIT;
  }
  if (!ri->errState && !ri->map.empty()) {
    for (auto mapi = ri->map.begin(); mapi != ri->map.end(); mapi++) {
      if (mapi->second && mapi->second->errState) {
        return mapi->second->errState;
      }
    }
  }
  return ri->errState;
}

const char* RetroHttp::getError() const {
  auto* ri = static_cast<RetroInternal*>(internal);
  if (!ri) {
    return "getError: must call ctorError first\n";
  }
  int n = getErrno();
  switch (n) {
    case RETRO_NO_ERROR:
      if (ri->map.empty()) {
        return "no error";
      }
      return "request in progress";
    case FAIL_IN_INIT:
      return "failed in init";
    case FAIL_VIA_WORKER_BAD_DATA:
      return "worker failed in data cb";
    case FAIL_VIA_HTTP_STATUS:
      return "server HTTP status code failed";
    case FAIL_DATA_CALLBACK:
      return "dataCb failed";
    default:
      return curl_multi_strerror(static_cast<CURLMcode>(n));
  }
}

long RetroHttp::getStatusCode() const {
  auto* ri = static_cast<const RetroInternal*>(internal);
  if (!ri) {
    return -1;
  }
  if (ri->map.empty()) {
    return -1;
  }
  return ri->map.begin()->second->httpCode;
}

const char* RetroHttp::getUrlPath() {
  auto* ri = static_cast<const RetroInternal*>(internal);
  if (!ri) {
    logE("RetroHttp::getUrlPath: must call ctorError first\n");
    return "getUrlPath: must call ctorError first\n";
  }
  if (ri->map.empty()) {
    logE("RetroHttp::getUrlPath: empty, must call ctorError first\n");
    return nullptr;
  }
  return ri->map.begin()->second->getUrlPath();
}

void RetroHttp::removeGET() {
  auto* ri = static_cast<RetroInternal*>(internal);
  if (!ri) {
    return;
  }
  for (auto mapi = ri->map.begin(); mapi != ri->map.end(); mapi++) {
    mapi->second->reset(ri->curlm);
  }
  ri->map.clear();
}

void RetroHttp::reset() {
  auto* ri = static_cast<RetroInternal*>(internal);
  if (!ri) {
    return;
  }
  for (auto mapi = ri->map.begin(); mapi != ri->map.end(); mapi++) {
    mapi->second->reset(ri->curlm);
  }
  ri->map.clear();
  if (ri->curlm) {
    curl_multi_cleanup(ri->curlm);
    ri->curlm = nullptr;
  }
  delete ri;
  internal = nullptr;
}

size_t RetroSocket::curlHeader(char* header, size_t len) {
  if (curl_easy_getinfo(curlHandle, CURLINFO_RESPONSE_CODE, &httpCode)) {
    logE("%s curl_easy_getinfo(CURLINFO_RESPONSE_CODE) failed\n", startedURL.c_str());
    return 0;
  }
  if (httpCode) {
    if ((httpCode / 100) > 3) {
      errState = RetroHttp::FAIL_VIA_HTTP_STATUS;
      return 0;  // Cause a CURLE_WRITE_ERROR.
    }
    if (0) {
      int n = len;
      while (n && (header[n - 1] == '\r' || header[n - 1] == '\n')) {
        n--;
      }
      logE("HTTP[%ld]: \"%.*s\"\n", httpCode, n, header);
    }
  }
  return len;
}

size_t RetroSocket::curlWrite(void* data, size_t len) {
  if (dataCb(dataCbSelf, data, len, size_t(getTotal > 0 ? getTotal : 0))) {
    errState = RetroHttp::FAIL_DATA_CALLBACK;
    return 0;
  }
  return len;
}
