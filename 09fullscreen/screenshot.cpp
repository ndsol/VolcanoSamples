/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 */

#include "screenshot.h"

// USE_D3D9: slow, B<->R reversed (easy to fix)
//
// Changes to run faster:
// * try using IDirect3DDevice9::GetRenderTarget and
// IDirect3DDevice9::GetRenderTargetData
//   in place of GetFrontBufferData?
//   see
//   https://blogs.msdn.microsoft.com/dsui_team/2013/03/25/ways-to-capture-the-screen/
//
// Comment this out to enable the DirectX 11 code - but that code does not work.
#define USE_D3D9

#if defined(_WIN32) && defined(USE_D3D9)
#include <d3d11.h>
#include <d3d9.h>
#include <dxgi.h>
#include <wrl/client.h>       /* for Microsoft::WRL::ComPtr */
using Microsoft::WRL::ComPtr; /* auto-Release() for COM objects */

#pragma comment(lib, "D3d11.lib")
#pragma comment(lib, "D3d9.lib")
#pragma comment(lib, "DXGI.lib")

typedef struct InternalData {
  GLFWmonitor* monitor;
  memory::Buffer* stage;
  void* stageMmap;
  HDC hdcScreen;
  HDC screenCompatibleDC;
  HBITMAP screenBmp;

  // d3d9 can capture the front buffer, but is slow.
  int useD3d9;
  ComPtr<IDirect3D9Ex> d3d9;
  ComPtr<IDirect3DDevice9> dev9;
  ComPtr<IDirect3DSurface9> surf9;
  UINT pitch;
  D3DDISPLAYMODE mode9;

  // The following are only used if d3d11 is used.
  // See
  // https://docs.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-opensharedresource
  // See
  // https://docs.microsoft.com/en-us/windows/win32/direct3darticles/surface-sharing-between-windows-graphics-apis
  int useD3d11;
  ComPtr<ID3D11Device> dev11;
  ComPtr<ID3D11DeviceContext> devCtx;
  ComPtr<IDXGISwapChain> swapChain;
  HANDLE sharedHnd;
  ComPtr<ID3D11Texture2D> tex2d;
  ComPtr<IDirect3DTexture9> tex9;
  ComPtr<IDirect3DSurface9> tex9surf;
  ComPtr<ID3D11Texture2D> tex2diface11;
  DXGI_MODE_DESC mode11;
  DXGI_SWAP_CHAIN_DESC scd;
} InternalData;

static int platformChooseMode(InternalData* i, ComPtr<IDXGIOutput> output) {
  static const std::vector<DXGI_FORMAT> formats{
      // see
      // https://docs.microsoft.com/en-us/windows-hardware/drivers/display/required-dxgi-formats
      DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
      DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
      DXGI_FORMAT_R8G8B8A8_UNORM,
      DXGI_FORMAT_R8G8B8A8_UINT,
  };
  const GLFWvidmode* mm = glfwGetVideoMode(i->monitor);

  for (auto f : formats) {
    DXGI_MODE_DESC wantMode;
    memset(&wantMode, 0, sizeof(wantMode));
    wantMode.Width = mm->width;
    wantMode.Height = mm->height;
    wantMode.Format = f;
    HRESULT hr =
        output->FindClosestMatchingMode(&wantMode, &i->mode11, i->dev11.Get());
    if (hr == DXGI_ERROR_NOT_FOUND) {
      continue;
    }
    if (FAILED(hr)) {
      logE("output->FindClosestMatchingMode failed: %x\n", hr);
      return 1;
    }
    return 0;
  }
  logE("output->FindClosestMatchingMode rejected all %zu formats\n",
       formats.size());
  return 1;
}

static int platformD3d11Init(InternalData* i) {
  const char* adapterPath = glfwGetWin32Adapter(i->monitor);
  size_t len = ::mbstowcs(nullptr, &adapterPath[0], 0);
  if (len == size_t(-1)) {
    logE("invalid adapterPath: \"%s\"\n", adapterPath);
    return 1;
  }
  std::wstring adapterWPath(len, 0);
  len = ::mbstowcs(&adapterWPath[0], &adapterPath[0], adapterWPath.size());
  if (len == size_t(-1)) {
    logE("unable to convert adapterPath: \"%s\"\n", adapterPath);
    return 1;
  }

  const GLFWvidmode* mm = glfwGetVideoMode(i->monitor);

  // Use DirectX11 API to match adapterPath - takes a lot of work.
  ComPtr<IDXGIFactory> factory;
  HRESULT hr =
      CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)factory.GetAddressOf());
  if (FAILED(hr)) {
    logE("CreateDXGIFactory failed: %x\n", hr);
    return 1;
  }
  bool modeValid = false;
  ComPtr<IDXGIOutput> output;
  ComPtr<IDXGIOutput> altOutput;
  for (UINT k = 0;; k++) {
    ComPtr<IDXGIAdapter> adapter;
    hr = factory->EnumAdapters(k, adapter.GetAddressOf());
    if (hr == DXGI_ERROR_NOT_FOUND) {
      if (k == 0) {
        logE("IDXGIFactory::EnumAdapters: 0 display adapters found\n");
        return 1;
      }
      break;  // Reached end of adapters
    }
    for (UINT j = 0;; j++) {
      hr = adapter->EnumOutputs(j, output.ReleaseAndGetAddressOf());
      if (hr == DXGI_ERROR_NOT_FOUND) {
        break;  // Reached end of outputs
      }
      if (FAILED(hr)) {
        logE("adapter->EnumOutputs(%d, %d) failed: %x\n", (int)k, (int)j, hr);
        return 1;
      }
      DXGI_OUTPUT_DESC outputDesc;
      hr = output->GetDesc(&outputDesc);
      if (FAILED(hr)) {
        logE("output->GetDesc(%d, %d) failed: %x\n", (int)k, (int)j, hr);
        return 1;
      }
      if (adapterWPath != outputDesc.DeviceName) {
        logE("skip output(%d, %d) \"%ls\" - want \"%s\"\n", (int)k, (int)j,
             outputDesc.DeviceName, adapterPath);
        if (altOutput == nullptr) {
          altOutput = output;
        }
        continue;
      }
      if (platformChooseMode(i, output)) {
        return 1;
      }
      if (modeValid) {
        logE("Found more than one valid output at (%d, %d)\n", (int)k, (int)j);
        continue;
      }
      modeValid = true;
    }
  }
  if (!modeValid) {
    if (altOutput) {
      logE("glfw adapter \"%s\" not found, fall back to first skipped result.",
           adapterPath);
      if (platformChooseMode(i, altOutput)) {
        return 1;
      }
      modeValid = true;
    } else {
      logE("IDXGIFactory::EnumAdapters/EnumOutputs: 0 display outputs found\n");
      return 1;
    }
  }

  memset(&i->scd, 0, sizeof(i->scd));
  memcpy(&i->scd.BufferDesc, &i->mode11, sizeof(i->scd.BufferDesc));
  i->scd.SampleDesc.Count = 1;
  i->scd.SampleDesc.Quality = 0;
  i->scd.BufferUsage = 0 | DXGI_USAGE_RENDER_TARGET_OUTPUT;
  i->scd.BufferCount = 1;
  i->scd.OutputWindow = GetDesktopWindow();
  i->scd.Windowed = TRUE;

  // NOTE: The DirectX API makes running around in circles unavoidable.
  // D3D11CreateDeviceAndSwapChain requires a NULL first arg (pAdapter) -
  // passing in adapter is, per the documentation, always an E_INVALIDARG.
  // So it chooses what *it* wants for factory and adapter. But *first* the API
  // requires this walk through all the adapters and their display outputs to
  // come up with i->scd.BufferDesc. Otherwise API will "helpfully" skip
  // i->monitor (And API completely lacks any way to query the current output.
  // Every app implements platformChooseMode which is only a guess at the mode.)
  UINT flags = 0;
  D3D_FEATURE_LEVEL feats[] = {
      // D3D_FEATURE_LEVEL_12_1,
      // D3D_FEATURE_LEVEL_12_0,
      // D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
  };
  D3D_FEATURE_LEVEL gotFeat;
  hr = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, feats,
      sizeof(feats) / sizeof(feats[0]), D3D11_SDK_VERSION, &i->scd,
      i->swapChain.GetAddressOf(), i->dev11.GetAddressOf(), &gotFeat,
      i->devCtx.GetAddressOf());
  if (FAILED(hr)) {
    logE("D3D11CreateDeviceAndSwapChain failed: %x\n", hr);
    return 1;
  }
  if (gotFeat < D3D_FEATURE_LEVEL_11_0) {
    logE("D3D11CreateDevice gotFeat %x vs 11_0=%x 11_1=%x 12_0=%x 12_1=%x\n",
         gotFeat, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
         D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1);
    return 1;
  }

  // Create a d3d11 texture2d. FIXME: this is not yet used.
  D3D11_TEXTURE2D_DESC texInfo;
  memset(&texInfo, 0, sizeof(texInfo));
  texInfo.Width = mm->width;
  texInfo.Height = mm->height;
  texInfo.MipLevels = 1;
  texInfo.ArraySize = 1;
  texInfo.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texInfo.SampleDesc.Count = 1;
  texInfo.SampleDesc.Quality = 0;
  // FIXME: D3D11_USAGE_DEFAULT can be used if the texture actually works.
  // and get rid of D3D11_CPU_ACCESS_READ.
  texInfo.Usage = D3D11_USAGE_STAGING;  // For readback by the CPU.
  texInfo.BindFlags = 0;
  texInfo.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  hr = i->dev11->CreateTexture2D(&texInfo, NULL, i->tex2d.GetAddressOf());
  if (FAILED(hr)) {
    logE("CreateTexture2D failed: %x\n", hr);
    return 1;
  }

  // Create a d3d9 texture and share it with d3d11.
  hr = i->dev9->CreateTexture(mm->width, mm->height, 1, D3DUSAGE_RENDERTARGET,
                              D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                              i->tex9.GetAddressOf(), &i->sharedHnd);
  if (FAILED(hr)) {
    logE("dev9->CreateTexture falied: %x\n", hr);
    return 1;
  }
  ComPtr<ID3D11Resource> viaResource;
  hr = i->dev11->OpenSharedResource(i->sharedHnd, __uuidof(ID3D11Resource),
                                    (void**)viaResource.GetAddressOf());
  if (FAILED(hr)) {
    logE("OpenSharedResource failed: %x\n", hr);
    return 1;
  }
  hr = viaResource->QueryInterface(i->tex2diface11.GetAddressOf());
  if (FAILED(hr)) {
    logE("QueryInterface(IDXGIResource) falied: %x\n", hr);
    return 1;
  }
  viaResource.Reset();

  hr = i->tex9->GetSurfaceLevel(0, i->tex9surf.GetAddressOf());
  if (FAILED(hr)) {
    logE("tex9->GetSurfaceLevel(0) falied: %x\n", hr);
    return 1;
  }
  return 0;
}

static int platformScreenshotInit(InternalData* i, memory::Image& out) {
  const GLFWvidmode* mm = glfwGetVideoMode(i->monitor);
  i->hdcScreen = GetDC(NULL);
  i->screenCompatibleDC = CreateCompatibleDC(i->hdcScreen);
  i->screenBmp = CreateCompatibleBitmap(i->hdcScreen, mm->width, mm->height);
  SelectObject(i->screenCompatibleDC, i->screenBmp);

  HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, i->d3d9.GetAddressOf());
  if (FAILED(hr)) {
    logE("Direct3DCreate9Ex failed\n");
    return 1;
  }
  UINT adapter = D3DADAPTER_DEFAULT;
  hr = i->d3d9->GetAdapterDisplayMode(adapter, &i->mode9);
  if (FAILED(hr)) {
    logE("GetAdapterDisplayMode failed\n");
    return 1;
  }
  if (i->mode9.Width != mm->width || i->mode9.Height != mm->height) {
    logE("GetAdapterDisplayMode: %dx%d want %dx%d (wrong monitor?)\n",
         (int)i->mode9.Width, (int)i->mode9.Height, (int)mm->width,
         (int)mm->height);
    return 1;
  }
  if (i->mode9.Width != out.info.extent.width ||
      i->mode9.Height != out.info.extent.height) {
    logE("GetAdapterDisplayMode: out is %dx%d want %dx%d\n",
         (int)out.info.extent.width, (int)out.info.extent.height,
         (int)i->mode9.Width, (int)i->mode9.Height);
    return 1;
  }

  D3DPRESENT_PARAMETERS fullscreen;
  memset(&fullscreen, 0, sizeof(fullscreen));
  fullscreen.Windowed = TRUE;
  fullscreen.BackBufferCount = 1;
  fullscreen.BackBufferHeight = i->mode9.Height;
  fullscreen.BackBufferWidth = i->mode9.Width;
  fullscreen.SwapEffect = D3DSWAPEFFECT_DISCARD;
  fullscreen.hDeviceWindow = NULL;

  hr = i->d3d9->CreateDevice(adapter, D3DDEVTYPE_HAL, NULL,
                             D3DCREATE_SOFTWARE_VERTEXPROCESSING, &fullscreen,
                             i->dev9.GetAddressOf());
  if (FAILED(hr)) {
    logE("CreateDevice failed\n");
    return 1;
  }
  VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
  hr = i->dev9->CreateOffscreenPlainSurface(i->mode9.Width, i->mode9.Height,
                                            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                            i->surf9.GetAddressOf(), nullptr);
  if (FAILED(hr)) {
    logE("CreateOffscreenPlainSurface failed\n");
    return 1;
  }

  D3DLOCKED_RECT rc;
  hr = i->surf9->LockRect(&rc, NULL, 0);
  if (FAILED(hr)) {
    logE("surface->LockRect failed\n");
    return 1;
  }
  i->pitch = rc.Pitch;
  hr = i->surf9->UnlockRect();
  if (FAILED(hr)) {
    logE("surface->UnlockRect failed\n");
    return 1;
  }
  i->stage = new memory::Buffer(out.vk.dev);
  i->stage->info.size = i->pitch * i->mode9.Height;
  i->stage->info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (i->stage->setName("screenshot.internal.stage") ||
      i->stage->ctorAndBindHostCoherent()) {
    logE("buffer stage failed\n");
    return 1;
  }
  if (i->stage->mem.mmap(&i->stageMmap)) {
    logE("buffer stage mmap failed\n");
    return 1;
  }
  if (out.vk && format != out.info.format) {
    logE("monitor format %u does not match image %u\n", (unsigned)format,
         (unsigned)out.info.format);
    return 1;
  }
  if (!out.vk) {
    // Create a new image
    out.info.format = format;
    if (out.ctorAndBindDeviceLocal()) {
      logE("out.ctorAndBindDeviceLocal failed\n");
      return 1;
    }
    logE("out created %u x %u\n", out.info.extent.width,
         out.info.extent.height);
  }
  i->useD3d9 = 1;

  if (!out.vk.dev.isExtensionLoaded(
          VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)) {
    logW("No %s, use slow path.\n",
         VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
    return 0;
  }

  if (platformD3d11Init(i)) {
    logE("platformD3d11Init failed\n");
    return 0;
  }
  i->useD3d11 = 0;
  if (out.vk.dev.isExtensionLoaded(
          VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME)) {
    logE("Can use %s\n", VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
    auto getBufProps =
        (PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR)vkGetDeviceProcAddr(
            out.vk.dev.dev, "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
    if (!getBufProps) {
      logE("getBufProps = NULL\n");
      return 1;
    }
    VkExternalBufferProperties VkInit(bufProps);
    auto& p = bufProps.externalMemoryProperties;

    VkPhysicalDeviceExternalBufferInfo VkInit(bufInfo);
    bufInfo.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    getBufProps(out.vk.dev.phys, &bufInfo, &bufProps);
    logE("HOST_ALLOC: feat=%x\n", p.externalMemoryFeatures);

    bufInfo.handleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT;
    getBufProps(out.vk.dev.phys, &bufInfo, &bufProps);
    logE("FOREIGN_MEM: feat=%x\n", p.externalMemoryFeatures);
    return 1;  // TODO: finish this feature
               // call vkGetMemoryFdPropertiesKHR too?
               //
               // "External memory handle types compatibility" in vulkan spec:
               // Only VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT
               // and
               // VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT
               // and VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT are linux
               // "read any mapping" options. The others all require the fd
               // comes from an identical driverUUID and deviceUUID.
               //
               // Create a buffer whose VkMemoryAllocateInfo has
               // pNext = &VkImportMemoryFdInfoKHR. Call dup2() because the fd
               // passed to Vulkan is then taken by Vulkan as if close() were
               // called on it. Vulkan then reads xShm's fd.
  }
  return 0;
}

static int _platformScreenshotWin32(InternalData* i, memory::Image& out,
                                    command::CommandBuffer& cmd) {
  HRESULT hr;
  if (i->useD3d11) {
    logE("not implemented\n");
    return 1;
  } else if (i->useD3d9) {
    hr = i->dev9->GetFrontBufferData(0, i->surf9.Get());
    if (FAILED(hr)) {
      logE("device->GetFrontBufferData failed: %x\n", hr);
      return 1;
    }
  } else {
    const GLFWvidmode* mm = glfwGetVideoMode(i->monitor);
    BitBlt(i->screenCompatibleDC, 0, 0, mm->width, mm->height, i->hdcScreen, 0,
           0, SRCCOPY);
    char headerWithPal[sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD)];
    BITMAPINFO& bmpInfo = *reinterpret_cast<BITMAPINFO*>(&headerWithPal[0]);
    memset(&bmpInfo, 0, sizeof(bmpInfo));
    bmpInfo.bmiHeader.biSize = sizeof(bmpInfo.bmiHeader);
    if (!GetDIBits(i->screenCompatibleDC, i->screenBmp, 0, mm->height, NULL,
                   &bmpInfo, DIB_RGB_COLORS)) {
      logE("GetDIBits() 1 of 2 failed\n");
      return 1;
    }
    if (bmpInfo.bmiHeader.biSizeImage > i->pitch * i->mode9.Height) {
      logE("GetDIBits() wants %u bytes, got %u\n",
           bmpInfo.bmiHeader.biSizeImage, i->pitch * i->mode9.Height);
      return 1;
    }
    if (!GetDIBits(i->screenCompatibleDC, i->screenBmp, 0, mm->height,
                   i->stageMmap, &bmpInfo, DIB_RGB_COLORS)) {
      logE("GetDIBits() 1 of 2 failed\n");
      return 1;
    }
  }

  if (i->useD3d11 || i->useD3d9) {
    D3DLOCKED_RECT rc;
    hr = i->surf9->LockRect(&rc, NULL, 0);
    if (FAILED(hr)) {
      logE("surface->LockRect failed: %x\n", hr);
      return 1;
    }
    memcpy(i->stageMmap, rc.pBits, rc.Pitch * i->mode9.Height);
    hr = i->surf9->UnlockRect();
    if (FAILED(hr)) {
      logE("surface->UnlockRect failed: %x\n", hr);
      return 1;
    }
  }

  VkBufferImageCopy copy;
  memset(&copy, 0, sizeof(copy));
  copy.bufferOffset = 0;
  copy.bufferRowLength = i->mode9.Width;
  copy.bufferImageHeight = i->mode9.Height;
  copy.imageSubresource = out.getSubresourceLayers(0);
  copy.imageSubresource.baseArrayLayer = 0;
  copy.imageSubresource.layerCount = 1;
  copy.imageOffset = {0, 0, 0};
  copy.imageExtent = {copy.bufferRowLength, copy.bufferImageHeight, 1};
  if (cmd.barrier(out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) ||
      cmd.copyImage(*i->stage, out, {copy})) {
    logE("screenshotGrab: copyImage failed\n");
    return 1;
  }
  return 0;
}

#elif defined(__APPLE__)
#include <MoltenVK/API/vk_mvk_moltenvk.h>
#include <dlfcn.h>

typedef struct InternalData {
  GLFWmonitor* monitor;
  void* getMTLTextureMVK;
  void* MVKHandle;
} InternalData;

// _platformScreenshotCocoa is in screenshot_cocoa.m
extern "C" int _platformScreenshotCocoa(GLFWmonitor* monitor, VkExtent3D ex,
                                        VkImage out, void* vkGetMTLTextureMVK);

static int platformScreenshotInit(InternalData* i, memory::Image& out) {
  logW("TODO: convert macOS colorspace from CG to vulkan VK_FORMAT\n");
  VkMVKObjectReturn objectReturn;
  memset(&objectReturn, 0, sizeof(objectReturn));
  objectReturn.sType = VK_MVK_STRUCTURE_TYPE_OBJECT_RETURN;
  objectReturn.handle = &i->MVKHandle;

  // Older MoltenVK versions used
  // vkGetDeviceProcAddr(out.mem.dev.dev, "vkGetMTLTextureMVK") but this is now
  // deprecated anticipating VK_GFX_metal - see
  // https://github.com/KhronosGroup/Vulkan-Loader/issues/146
  void* mvkDylib =
      dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
  if (!mvkDylib) {
    logE("dlopen(%s) failed: %s\n", "libMoltenVK.dylib", dlerror());
    return 1;
  }
  i->getMTLTextureMVK = dlsym(mvkDylib, "vkGetMTLTextureMVK");
  if (!i->getMTLTextureMVK) {
    logE("dlsym(vkGetMTLTextureMVK) failed: %s\n", dlerror());
    if (dlclose(mvkDylib)) {
      logE("dlclose(%s) failed: %s\n", "libMoltenVK.dylib", dlerror());
    }
    return 1;
  }
  if (dlclose(mvkDylib)) {
    logE("dlclose(%s) failed: %s\n", "libMoltenVK.dylib", dlerror());
    return 1;
  }
  VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
  if (out.vk && format != out.info.format) {
    logE("screenshotInit: monitor format %u does not match image %u\n",
         (unsigned)format, (unsigned)out.info.format);
    return 1;
  }
  if (out.vk) {
    return 0;
  }

  // Create a new image
  out.info.format = format;
  out.info.pNext = &objectReturn;
  if (out.ctorAndBindDeviceLocal()) {
    logE("out.ctorAndBindDeviceLocal failed\n");
    return 1;
  }
  out.info.pNext = NULL;
  if (!i->MVKHandle) {
    logE("out.ctor did not return MVKHandle - turn OFF validation layers\n");
    return 1;
  }
  logE("out created %u x %u\n", out.info.extent.width, out.info.extent.height);
  return 0;
}

#elif defined(__ANDROID__)
#error Android not implemented yet
#elif defined(__linux__)
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <xcb/shm.h>

enum InternalShmMode {
  UNKNOWN = 0,
  // In xGetImage mode, the bitmap must be sent over the Xlib socket (slow!)
  // 1. GPU drver copies bitmap from GPU memory to CPU
  // 2. CPU sends it a byte at a time over a socket (slow!) X server -> client
  //    into a Vulkan buffer for staging
  // 3. GPU driver copies it from the staging buffer to GPU memory
  OLD_XGETIMAGE,
  // If the MIT-SHM extension is present, the bitmap is available but only
  // in Sys V IPC shmget() memory, which Vulkan cannot directly import.
  // 1. GPU driver copies bitmap from GPU memory to CPU shmget() memory
  // 2. This code copies from shmget() memory to a Vulkan buffer for staging
  // 3. GPU driver copies it from the staging buffer to GPU memory
  XMIT_SHM,
  // If xcb has the SHM_FD extension, Vulkan imports the fd (file descriptor)
  // 1. GPU driver makes a file descriptor (fd)
  // 2. GPU driver copies bitmap from GPU memory to CPU mmaped fd
  // 3. GPU driver copies bitmap from CPU mmaped fd to GPU memory
  // (2 copies are better than XMIT_SHM's 3 copies. But also, the copies
  // are all done by the same driver so there is a chance for the GPU to use
  // its transfer engine. The GPU may even cache the data.)
  XCB_FD,
};

typedef struct InternalData {
  GLFWmonitor* monitor;
  XImage* im;
  size_t len;
  Display* display;
  int screen;
  XShmSegmentInfo shminfo;
  int xofs, yofs;
  InternalShmMode shmMode;
  memory::Buffer* stage;
  void* stageMmap;

  // xcb members are only used if shmMode == XCB_FD:
  int xcb_fd;
} InternalData;

static int platformScreenshotX11(InternalData* i, memory::Image& out,
                                 command::CommandBuffer& cmd) {
  if (!i->stageMmap) {
    logE("screenshotGrab: stage.mmap failed\n");
    return 1;
  }

  Display* dpy = i->display;
  Window root = RootWindow(dpy, i->screen);
  switch (i->shmMode) {
    case UNKNOWN:
      logE("screenshotInit: BUG shmMode is unknown\n");
      return 1;
    case OLD_XGETIMAGE: {
      LockDisplay(dpy);
      xGetImageReq* req;
      GetReq(GetImage, req);

      /* First set up the standard stuff in the request */
      req->drawable = (Drawable)root;
      req->x = i->xofs;
      req->y = i->yofs;
      req->width = i->im->width;
      req->height = i->im->height;
      req->planeMask = (unsigned int)AllPlanes;
      req->format = ZPixmap;

      xGetImageReply rep;
      if (!_XReply(dpy, (xReply*)&rep, 0, xFalse) || !rep.length) {
        UnlockDisplay(dpy);
        SyncHandle();
        logE("X: GetImage failed\n");
        return 1;
      }

      long nbytes = (long)rep.length << 2;
      _XReadPad(dpy, (char*)i->stageMmap, nbytes);

      UnlockDisplay(dpy);
      SyncHandle();
      break;
    }
    case XMIT_SHM:
    case XCB_FD:
      if (!XShmGetImage(dpy, root, i->im, i->xofs, i->yofs, AllPlanes)) {
        logE("XShmGetImage failed\n");
        return 1;
      }

      if (i->im->bits_per_pixel == 24) {
        // Insert alpha channel when reading from 24bpp visual.
        uint32_t* dst = (uint32_t*)i->stageMmap;
        uint8_t* src = (uint8_t*)i->im->data;
        for (unsigned y = 0; y < out.info.extent.height; y++) {
          for (unsigned x = 0; x < out.info.extent.width; x++) {
            uint32_t c = 0xff000000lu;
            c |= *(src++);
            c |= (uint32_t)(*(src++)) << 8;
            c |= (uint32_t)(*(src++)) << 16;
            *(dst++) = c;
          }
        }
      } else {
        size_t len = i->len;
        if (len > i->stage->info.size) {
          logW("screenshotGrab: stage.size = %zu want %zu\n",
               (size_t)i->stage->info.size, len);
          len = i->stage->info.size;
        }
        memcpy(i->stageMmap, i->im->data, len);
      }
      break;
  }
  VkBufferImageCopy copy;
  memset(&copy, 0, sizeof(copy));
  copy.bufferOffset = 0;
  copy.bufferRowLength = out.info.extent.width;
  copy.bufferImageHeight = out.info.extent.height;
  copy.imageSubresource = out.getSubresourceLayers(0);
  copy.imageSubresource.baseArrayLayer = 0;
  copy.imageSubresource.layerCount = 1;
  copy.imageOffset = {0, 0, 0};
  copy.imageExtent = {copy.bufferRowLength, copy.bufferImageHeight, 1};
  if (cmd.barrier(out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) ||
      cmd.copyImage(*i->stage, out, {copy})) {
    logE("screenshotGrab: copyImage failed\n");
    return 1;
  }
  return 0;
}

static VkFormat formatOfXImage(XImage* image) {
  switch (image->bits_per_pixel) {
    case 8:
      return VK_FORMAT_R8_SRGB;
    case 16:
      if (image->red_mask == 0xf800 && image->green_mask == 0x07e0 &&
          image->blue_mask == 0x001f) {
        return VK_FORMAT_R5G6B5_UNORM_PACK16;
      } else if (image->red_mask == 0x7c00 && image->green_mask == 0x03e0 &&
                 image->blue_mask == 0x001f) {
        return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
      }
      break;
    case 24:  // 24bpp images will be upconverted below.
    case 32:
      if (image->red_mask == 0xff0000 && image->green_mask == 0x00ff00 &&
          image->blue_mask == 0x0000ff) {
        return VK_FORMAT_B8G8R8A8_SRGB;
      } else if (image->red_mask == 0x0000ff && image->green_mask == 0x00ff00 &&
                 image->blue_mask == 0xff0000) {
        return VK_FORMAT_R8G8B8A8_SRGB;
      }
      break;
    default:
      break;
  }
  return VK_FORMAT_UNDEFINED;
}

static VkDeviceSize sizeOfXImage(XImage* image) {
  switch (image->bits_per_pixel) {
    case 8:
      return 1;
    case 16:
      return 2;
    case 24:  // 24bpp images will be upconverted below.
    case 32:
      return 4;
    default:
      break;
  }
  return 0;
}

static int platformXErrorCount = 0;
static int platformHandleXError(Display* display, XErrorEvent* event) {
  (void)display;
  (void)event;
  platformXErrorCount++;
  return 0;
}

static int platformScreenshotInit(InternalData* i, memory::Image& out) {
  i->display = glfwGetX11Display();
  i->screen = DefaultScreen(i->display);
  if (DisplayWidth(i->display, i->screen) != (int)out.info.extent.width ||
      DisplayHeight(i->display, i->screen) != (int)out.info.extent.height) {
    logE("screenshotInit: got %d x %d, want %d x %d\n",
         DisplayWidth(i->display, i->screen),
         DisplayHeight(i->display, i->screen), (int)out.info.extent.width,
         (int)out.info.extent.height);
    return 1;
  }
  i->shmMode = OLD_XGETIMAGE;
  if (XShmQueryExtension(i->display)) {
    i->shmMode = XMIT_SHM;
    int shmMajor = 0, shmMinor = 0;
    Bool pixmaps;
    if (!XShmQueryVersion(i->display, &shmMajor, &shmMinor, &pixmaps)) {
      shmMajor = 0;
      shmMinor = 0;
    }
    if (shmMajor > 1 || (shmMajor == 1 && shmMinor >= 2)) {
      i->shmMode = XCB_FD;
    }
  }
  if (out.vk.dev.isExtensionLoaded(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
    logE("Can use %s\n", VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    if (out.vk.dev.isExtensionLoaded(
            VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME)) {
      logE("Can use %s\n", VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
      auto getBufProps = (PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR)
          vkGetDeviceProcAddr(out.vk.dev.dev,
                              "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
      if (!getBufProps) {
        logE("getBufProps = NULL\n");
        return 1;
      }
      VkExternalBufferProperties VkInit(bufProps);
      auto& p = bufProps.externalMemoryProperties;

      VkPhysicalDeviceExternalBufferInfo VkInit(bufInfo);
      bufInfo.usage =
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      bufInfo.handleType =
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
      getBufProps(out.vk.dev.phys, &bufInfo, &bufProps);
      logE("HOST_ALLOC: feat=%x\n", p.externalMemoryFeatures);

      bufInfo.handleType =
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT;
      getBufProps(out.vk.dev.phys, &bufInfo, &bufProps);
      logE("FOREIGN_MEM: feat=%x\n", p.externalMemoryFeatures);
      return 1;  // TODO: finish this feature
      // call vkGetMemoryFdPropertiesKHR too?
      //
      // "External memory handle types compatibility" in vulkan spec:
      // Only VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT
      // and VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT
      // and VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT are linux "read any
      // mapping" options. The others all require the fd comes from an
      // identical driverUUID and deviceUUID.
      //
      // Create a buffer whose VkMemoryAllocateInfo has
      // pNext = &VkImportMemoryFdInfoKHR. Call dup2() because the fd passed
      // to Vulkan is then taken by Vulkan as if close() were called on it.
      // Vulkan then reads xShm's fd.
    }
  }
  switch (i->shmMode) {
    case UNKNOWN:
      logE("screenshotInit: BUG shmMode is unknown\n");
      return 1;
    case XMIT_SHM:
    case XCB_FD:
      // Both XMIT_SHM and XCB_FD start down the same pathway.
      // If XCB_FD is present but it fails, fall back to XMIT_SHM.
      i->im = XShmCreateImage(i->display, DefaultVisual(i->display, i->screen),
                              DefaultDepth(i->display, i->screen), ZPixmap,
                              NULL, &i->shminfo, out.info.extent.width,
                              out.info.extent.height);
      if (!i->im) {
        logE("screenshotInit: XShmCreateImage failed\n");
        return 1;
      }
      i->len = i->im->bytes_per_line * out.info.extent.height;

      if (i->shmMode == XCB_FD) {
        // Attempt to xcb_shm_create_segment and then mmap it.
        // https://git.enlightenment.org/legacy/imlib2.git/commit/?id=ca17031280fddf1ba14a5f9d8d89507301d0db26
        xcb_connection_t* xcb_conn = XGetXCBConnection(i->display);
        i->shminfo.shmseg = xcb_generate_id(xcb_conn);
        i->shminfo.readOnly = False;
        xcb_shm_create_segment_cookie_t cookie = xcb_shm_create_segment(
            xcb_conn, i->shminfo.shmseg, i->len, i->shminfo.readOnly);
        xcb_generic_error_t* error = NULL;
        xcb_shm_create_segment_reply_t* reply =
            xcb_shm_create_segment_reply(xcb_conn, cookie, &error);
        if (reply && reply->nfd == 1) {
          int* fds = xcb_shm_create_segment_reply_fds(xcb_conn, reply);
          i->shminfo.shmaddr = (char*)mmap(NULL, i->len, PROT_READ | PROT_WRITE,
                                           MAP_SHARED, fds[0], 0);
          i->xcb_fd = fds[0];
          if (i->shminfo.shmaddr == MAP_FAILED) {
            i->shminfo.shmaddr = NULL;
          }
          if (i->shminfo.shmaddr) {
            i->im->data = i->shminfo.shmaddr;
            free(error);
            break;
          } else {  // if (i->shminfo.shmaddr)
            xcb_shm_detach(xcb_conn, i->shminfo.shmseg);
            logW("XCB_FD: mmap(fd %d) failed: %d %s\n", fds[0], errno,
                 strerror(errno));
          }
        } else {
          logW("XCB_FD: xcb_shm_create_segment_reply failed\n");
        }
        free(error);  // Ignore error details.
        error = NULL;

        // Fall back to XMIT_SHM.
        memset(&i->shminfo, 0, sizeof(i->shminfo));
        i->shmMode = XMIT_SHM;
      }
      i->shminfo.shmid = shmget(IPC_PRIVATE, i->len, IPC_CREAT | 0777);
      if (i->shminfo.shmid == -1) {
        logE("screenshotInit: shmget failed: %d %s\n", errno, strerror(errno));
        i->shminfo.shmid = 0;
        return 1;
      }
      i->im->data = (decltype(i->im->data))shmat(i->shminfo.shmid, 0, 0);
      i->shminfo.shmaddr = i->im->data;
      if (!i->shminfo.shmaddr) {
        logE("screenshotInit: shmat failed: %d %s\n", errno, strerror(errno));
        return 1;
      }
      XSync(i->display, False);
      platformXErrorCount = 0;
      {
        XErrorHandler prevErrorHandler = XSetErrorHandler(platformHandleXError);
        if (!XShmAttach(i->display, &i->shminfo)) {
          logE("screenshotInit: XShmAttach failed\n");
          return 1;
        }
        XSync(i->display, False);
        XSetErrorHandler(prevErrorHandler);
      }
      if (!platformXErrorCount) {
        break;
      }

      logW("screenshotInit: XShmAttach failed. Using old XGetImage.\n");
      logW("screenshotInit: this usually means a remote X connection.\n");
      shmdt(i->shminfo.shmaddr);
      shmctl(i->shminfo.shmid, IPC_RMID, NULL);
      // Fall through.
    case OLD_XGETIMAGE:
      i->im = XGetImage(i->display, RootWindow(i->display, i->screen), i->xofs,
                        i->yofs, out.info.extent.width, out.info.extent.height,
                        AllPlanes, ZPixmap);
      if (!i->im) {
        logE("screenshotInit: XGetImage failed\n");
        return 1;
      }
      break;
  }

  if (0) {
    // Use xrandr info provided by glfw to pick the current screen
    RRCrtc adapter = glfwGetX11Adapter(i->monitor);
    (void)adapter;
    RROutput output = glfwGetX11Monitor(i->monitor);
    (void)output;
  }

  VkFormat format = formatOfXImage(i->im);
  VkDeviceSize formatSize = sizeOfXImage(i->im);
  if (out.vk && format != out.info.format) {
    logE("screenshotInit: monitor format %u does not match image %u\n",
         (unsigned)format, (unsigned)out.info.format);
    return 1;
  }
  if (!i->stage) {
    i->stage = new memory::Buffer(out.vk.dev);
    i->stage->info.size =
        formatSize * out.info.extent.width * out.info.extent.height;
    i->stage->info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (i->stage->setName("screenshot.internal.stage") ||
        i->stage->ctorAndBindHostCoherent()) {
      logE("screenshotInit: buffer stage failed\n");
      return 1;
    }
    if (i->stage->mem.mmap(&i->stageMmap)) {
      logE("screenshotInit: buffer stage mmap failed\n");
      return 1;
    }
  }
  if (out.vk) {
    return 0;
  }

  // Create a new image
  out.info.format = format;
  if (out.ctorAndBindDeviceLocal()) {
    logE("out.ctorAndBindDeviceLocal failed\n");
    return 1;
  }
  logE("out created %u x %u\n", out.info.extent.width, out.info.extent.height);
  return 0;
}

#else
#error unsupported platform
#endif

void* screenshotInit(GLFWmonitor* monitor, memory::Image& out) {
  const GLFWvidmode* mode = glfwGetVideoMode(monitor);
  float xscale, yscale;
#ifdef GLFW_EXPOSE_NATIVE_X11
  xscale = 1.;  // Not all monitors report correctly on Linux.
  yscale = 1.;
#else
  glfwGetMonitorContentScale(monitor, &xscale, &yscale);
#endif
  float wf = mode->width * xscale;
  float hf = mode->height * yscale;
  if (wf < 1 || hf < 1) {
    logE("screenshotInit: invalid monitor: %f x %f\n", wf, hf);
    return NULL;
  }
  uint32_t w = (uint32_t)wf, h = (uint32_t)hf;
  if (out.vk) {
    if (out.info.extent.width != w || out.info.extent.height != h) {
      logW("screenshotInit: monitor %u x %u does not match out %u x %u\n", w, h,
           out.info.extent.width, out.info.extent.height);
      out.vk.reset();  // Destroy out.vk, then create it.
    } else if (out.info.extent.depth != 1) {
      logW("screenshotInit: image is an array of %u, using only the first\n",
           out.info.extent.depth);
    }
  } else {
    out.info.extent.width = w;
    out.info.extent.height = h;
    out.info.extent.depth = 1;
    out.info.usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }

  auto* i = (InternalData*)calloc(sizeof(InternalData), 1);
  if (!i) {
    logE("screenshotInit: calloc(InternalData) failed\n");
    return NULL;
  }
  i->monitor = monitor;
  if (platformScreenshotInit(i, out)) {
    screenshotFree((void*)i);
    return NULL;
  }
  return i;
}

int screenshotGrab(void* opaque, memory::Image& out,
                   command::CommandBuffer& cmd) {
  if (!opaque) {
    logE("screenshotGrab: screenshotInit failed\n");
    return 1;
  }

  InternalData* i = reinterpret_cast<InternalData*>(opaque);
#ifdef _WIN32
  return _platformScreenshotWin32(i, out, cmd);
#elif defined(__APPLE__)
  return _platformScreenshotCocoa(i->monitor, out.info.extent,
                                  (VkImage)i->MVKHandle, i->getMTLTextureMVK);
#elif defined(__linux__) && !defined(__ANDROID__)
  return platformScreenshotX11(i, out, cmd);
#endif
}

void screenshotFree(void* opaque) {
  if (!opaque) {
    return;
  }
  InternalData* i = reinterpret_cast<InternalData*>(opaque);
#if defined(_WIN32) && defined(USE_D3D9)
  if (i->stageMmap) {
    i->stage->mem.munmap();
  }
  if (i->stage) {
    delete i->stage;
  }
  i->tex9surf.Reset();
  i->tex9.Reset();
  i->tex2diface11.Reset();
  i->surf9.Reset();
  i->dev9.Reset();
  i->d3d9.Reset();
  i->tex2d.Reset();
  i->swapChain.Reset();
  i->devCtx.Reset();
  i->dev11.Reset();
  DeleteObject(i->screenCompatibleDC);
#elif defined(__APPLE__)
#elif defined(__linux__) && !defined(__ANDROID__)
  if (i->stageMmap) {
    i->stage->mem.munmap();
  }
  if (i->stage) {
    delete i->stage;
  }
  switch (i->shmMode) {
    case UNKNOWN:
    case OLD_XGETIMAGE:
      break;
    case XMIT_SHM:
      XShmDetach(i->display, &i->shminfo);
      if (i->shminfo.shmaddr) {
        shmdt(i->shminfo.shmaddr);
      }
      if (i->shminfo.shmid) {
        shmctl(i->shminfo.shmid, IPC_RMID, NULL);
      }
      break;
    case XCB_FD:
      munmap(i->im->data, i->len);
      if (i->xcb_fd) {
        close(i->xcb_fd);
      }
      if (i->shminfo.shmseg) {
        xcb_connection_t* xcb_conn = XGetXCBConnection(i->display);
        xcb_shm_detach(xcb_conn, i->shminfo.shmseg);
      }
      break;
  }
  if (i->im) {
    XDestroyImage(i->im);
  }
#endif
  free(i);
}
