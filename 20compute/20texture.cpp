/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * This sample demonstrates GPU-accelerated texture compression.
 */

#include "20texture.h"

namespace vert {
#include "20compute/struct_20texture.vert.h"
}  // namespace vert

namespace example {

const int INIT_WIDTH = 800, INIT_HEIGHT = 600;

Compressor::Compressor(language::Instance& instance, GLFWwindow* window)
    : BaseApplication{instance},
      uglue{*this, window, 0 /*maxLayoutIndex*/,
            vert::bindingIndexOfUniformBufferObject(),
            sizeof(vert::UniformBufferObject)},
      textureGPU(*this) {
  resizeFramebufListeners.emplace_back(std::make_pair(
      [](void* self, language::Framebuf& framebuf, size_t fbi, size_t) -> int {
        return static_cast<Compressor*>(self)->buildFramebuf(framebuf, fbi);
      },
      this));
  uglue.redrawListeners.emplace_back(std::make_pair(
      [](void* self, std::shared_ptr<memory::Flight>& flight) -> int {
        return static_cast<Compressor*>(self)->redraw(flight);
      },
      this));
};

int Compressor::run(const Options& options) {
  if (textureGPU.decoder.open(options.filename.c_str(),
                              textureGPU.computeStage.mmapMax())) {
    return 1;
  }
  if (!textureGPU.decoder.codec) {
    logE("initCompute: must call textureGPU.decoder.open() first.\n");
    return 1;
  }
  auto& ci = textureGPU.decoder.codec->getInfo();
  if (ci.width() < 1 || ci.height() < 1) {
    logE("initCompute: must call decoder.open() first. %d x %d is invalid.\n",
         ci.width(), ci.height());
    return 1;
  }
  textureGPU.inputW = ci.width();
  textureGPU.inputH = ci.height();

  if (initUI() || initCompute()) {
    return 1;
  }

  for (;;) {
    if (!uglue.window) {
      // FIXME: if compression is done, exit.
      continue;
    }
    if (textureGPU.poll()) {
      return 1;
    }
    if (textureGPU.workDone() || uglue.windowShouldClose()) {
      break;
    }
    UniformGlue::onGLFWRefresh(uglue.window);  // Calls redraw().
    if (uglue.redrawErrorCount > 0) {
      return 1;
    }
  }
  if (cpool.deviceWaitIdle()) {
    logE("cpool.deviceWaitIdle() failed\n");
    return 1;
  }
  return 0;
}

static int createApp(GLFWwindow* window, const Options& options) {
  int width, height, r = 1;  // Let GLFW-on-Android override the window size.
  unsigned int eCount = 0;
  const char** e = nullptr;
  if (!window) {
    width = INIT_WIDTH;
    height = INIT_HEIGHT;
  } else {
    glfwGetWindowSize(window, &width, &height);
    e = glfwGetRequiredInstanceExtensions(&eCount);
  }
  language::Instance inst;
  if (!window) {
    // Tell Volcano devices without PRESENT are ok. VK_KHR_swapchain is not
    // needed. Volcano will still enable VK_KHR_swapchain if present as a
    // convenience, but ignore that.
    if (!inst.minSurfaceSupport.erase(language::PRESENT)) {
      logE("removing PRESENT from minSurfaceSupport: not found\n");
      return 1;
    }
  } else {
    inst.requiredExtensions.insert(inst.requiredExtensions.end(), e,
                                   &e[eCount]);
  }
  if (inst.ctorError(
          [](language::Instance& inst, void* window) -> VkResult {
            if (!window) {
              return VK_SUCCESS;
            }
            return glfwCreateWindowSurface(inst.vk, (GLFWwindow*)window,
                                           inst.pAllocator, &inst.surface);
          },
          window) ||
      // inst.open() takes a while, especially if validation layers are on.
      inst.open({(uint32_t)width, (uint32_t)height})) {
    logE("inst.ctorError or inst.open failed\n");
  } else if (!inst.devs.size()) {
    logE("No vulkan devices found (or driver missing?)\n");
  } else {
    if (inst.devs.size() > 1) {
      logI("Multiple GPUs available, using dev[0]:\n");
      for (size_t i = 0; i < inst.devs.size(); i++) {
        logI("dev[%zu] \"%s\"\n", i,
             inst.devs.at(i)->physProp.properties.deviceName);
      }
    }
    r = std::make_shared<Compressor>(inst, window)->run(options);
  }
  return r;  // Destroy inst only after app.
}

static int usage(char** argv) {
  logE("Usage: %s [--tui] [--quality Q] input-image\n", argv[0]);
  logE("       --tui: use console only\n");
  logE("       --quality Q: Q can be from 0-9\n");
  logE("                    0 = worst quality (fastest compression)\n");
  logE("                    6 = best quality (slowest compression)\n");
  logE("                    7+ are the same as 6 at present\n");
  return 1;
}

static int crossPlatformMain(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--tui")) {
      options.flagTui = true;
    } else if (!strncmp(argv[i], "-q", 2)) {
      // Parse argv[i] as "-q%d"
      int n;
      if (sscanf(argv[i], "-q%d%n", &options.quality, &n) != 1 ||
          n != (int)strlen(argv[i]) || options.quality < 0 ||
          options.quality > 9) {
        return usage(argv);
      }
    } else if (!strcmp(argv[i], "--quality")) {
      // Parse argv[i] "--quality" and argv[i + 1] as "%d"
      i++;
      if (i >= argc) {
        return usage(argv);
      }
      int n;
      if (sscanf(argv[i], "%d%n", &options.quality, &n) != 1 ||
          n != (int)strlen(argv[i]) || options.quality < 0 ||
          options.quality > 9) {
        return usage(argv);
      }
    } else if (options.filename.empty()) {
      options.filename = argv[i];
    } else {
      return usage(argv);
    }
  }
  if (options.filename.empty()) {
    return usage(argv);
  }
  GLFWwindow* window = nullptr;
  if (!options.flagTui) {
    if (!glfwInit()) {
      logW("glfwInit failed. Using --tui mode.\n");
      options.flagTui = true;
    }
  }
  if (!options.flagTui) {
    glfwSetErrorCallback([](int code, const char* msg) -> void {
      logE("glfw error %x: %s\n", code, msg);
    });
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window = glfwCreateWindow(
        INIT_WIDTH, INIT_HEIGHT, "20texture Vulkan window",
        nullptr /*monitor for fullscreen*/, nullptr /*context object sharing*/);
  }
  int r = createApp(window, options);
  if (!options.flagTui) {
    glfwDestroyWindow(window);
    glfwTerminate();
  }
  return r;
}

}  // namespace example

// OS-specific startup code.
#ifdef _WIN32
// Windows-specific startup.
int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  return example::crossPlatformMain(__argc, __argv);
}
#elif defined(__ANDROID__)
#error 20texture is a host-only tool.
#else
// Posix startup.
int main(int argc, char** argv) {
  return example::crossPlatformMain(argc, argv);
}
#endif
