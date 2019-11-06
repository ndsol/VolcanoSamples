<table cellspacing="0" cellpadding="0"><tbody>
<tr valign="top"><td width="60%" colspan="2">

# Volcano Sample 4: Android + GLFW

This sample uses [Volcano](https://github.com/ndsol/volcano) to draw a line for
every mouse click or multi-touch gesture on the screen. This demonstrates
Android support and simple use of a vertex buffer.

</td></tr><tr valign="top"><td width="50%">

[View source code](./)

1. [Goals](#goals)
1. [Why Support Android Everywhere?](#why-support-android-everywhere)
   1. [Getting to an Android .apk](#getting-to-an-android-apk)
1. [Android Activity Lifecycle](#android-activity-lifecycle)
   1. [App state](#app-state)
1. [GLFW and Android](#glfw-and-android)
1. [How To Set Up a Vertex Buffer](#how-to-set-up-a-vertex-buffer)
   1. [Vulkan Memory Allocator](#vulkan-memory-allocator)
1. [Shader Reflection Preview](#shader-reflection-preview)
1. [Line Drawing (Vulkan State Changes)](#line-drawing-vulkan-state-changes)
1. [Logging)(#logging)
1. [The End](#the-end)

</td><td width="50%">

![Screenshot](screenshot.png)

[The top level README](../) shows how to build this sample on a desktop PC.
See below for how to build the Android app.

Run this sample on a desktop by typing:<br/>`out/Debug/04android`

Enable Vulkan Validation layers by typing:<br/>
`VK_INSTANCE_LAYERS=\`<br/>
`VK_LAYER_LUNARG_standard_validation \`<br/>
`out/Debug/04android`</tr></tbody></table>

## Goals

This sample should teach you:

1. How to run an app on both desktop and Android, built using a single source
   tree.

1. How OpenGL and Vulkan state updates are similar and how they differ.

## Why Support Android Everywhere?

[Volcano](https://github.com/ndsol/volcano) and Vulkan are great for
cross-platform development. Develop your app on your desktop, then instantly
cross-compile it as an Android app. All samples 4+, and also
[Sample 1](../01glfw/README.md), can run on Android.

Your desktop-class GPU will blow a handheld device out of the water. A large
part of that is that it can burn hundreds of watts while your phone can't. But
Volcano supports all Vulkan platforms. You can at least try your app on a phone
and see how it fares.

This is not a tutorial on how to create a best-selling game in the Play Store.
This makes a nifty demo app that uses Vulkan. That is all.

### Getting to an Android .apk

**Apologies, Windows Developers!** Please get a Linux machine, VM image,
or Mac OS desktop - Android builds on Windows are not yet supported.
(We're working on it!)

Type `cd 04android` then `./build-android.py`.

This is the workflow you can use to iterate on an Android app:

1. `cd 04android` because the build-android.py script uses the current dir
   to know what to name the result. Feel free to make your own dir and start
   experimenting.
1. `./build-android.py` spits out `../out/Debug/04android.apk`.<br/>
   It gets the Android SDK and NDK, then runs ninja for you.
1. `adb install -r ../out/Debug/04android.apk` installs it on your device.
1. `adb logcat -c` then `adb logcat` to start a log capture. Do this in a
   different window.
1. `adb shell am start -N com.ndsol.VolcanoSample` to remotely run the app on
   the phone.
1. `adb uninstall com.ndsol.VolcanoSample` remotely uninstalls the app.

<details>
<summary><b>Q:</b> What about debugging in Android Studio?
[Click to expand]</summary>

**tl;dr:** Limited support.

Volcano aims to minimize download pain; `build-android.py` downloads the latest
Android NDK and SDK instead of the full-blown Android Studio. It also updates
the NDK and SDK versions less aggressively. Hopefully this lets you focus on
Vulkan and your app and just gets out of your way.

`build-android.py` automatically generates an Android Studio project from the
`BUILD.gn` file in the current directory. The Android Studio project is written
to `out/Debug` by appending `-droid` to the path:

* Run `cd 01glfw && build-android.py` to create `out/Debug/01glfw-droid`
* Run `cd 04android && build-android.py` to create `out/Debug/04android-droid`
* Run `cd 05indexbuffer && build-android.py` to create
  `out/Debug/05indexbuffer-droid`
* etc.

In that out/Debug/*-droid directory is a full Android Studio project.
You will have to manually download Android Studio, the platform utilities, the
many updates, etc. In Android Studio, choose "Open project..." and
select the out/Debug/*-droid directory for your app.

**Warning:** Any edits you make in android studio are blindly nuked by
`build-android.py` without any warning the next time you use Volcano to build
the app.

Android Studio's integrated debugger is super useful for debugging native code.

However, the android studio tree shows up incorrectly in the debugger. Use the
command line tool, `adb`, instead.

</details><details>
<summary><b>Q:</b> Do Android emulators support Vulkan?
[Click to expand]</summary>

**Answer:** At this time, no. There are projects just getting started to
implement Vulkan entirely on the host or translate Vulkan calls using an
instance layer.

</details>

## Android Activity Lifecycle

![Android Activity Lifecycle](androidactivity.png)

(CC BY 2.5,
[Android Activity Lifecycle](https://developer.android.com/guide/components/activities/activity-lifecycle.html))

Android apps live in a very different world. If you noticed this sample
restarts from scratch every time you drop to the Android home screen, read on.

In Android, an app can be destroyed at any time. Android needs apps to be ready
at any time to reduce their memory usage, even exit completely. Not all app
switches occur at expected times: an incoming text or phone call, a battery
notification, or a popup can instantly result in your app exiting.

This makes android apps harder to design. You are probably used to the C++ app
style where important variables are stored on the stack and the main loop runs
for the entire life of the app - the result is desktop C++ apps do not exit
and restart instantaneously. They don't have to.

Android apps do.

Ok, technically
[many Android apps do not throw away](https://johanneskuhlmann.de/)
the entire Vulkan instance, all loaded textures, etc. if the app loses focus.
[They are optimized](https://www.gamasutra.com/blogs/JohannesKuhlmann/20171016/307643/Bringing_Galaxy_on_Fire_3_to_Vulkan_Vulkan_on_Android.php)
to only destroy two objects: when the app loses focus, it must destroy the
`Device::surface`. As a result, the `Device::swapChain` must also be
destroyed.

```C++
  static void onGLFWFocus(GLFWwindow* window, int focused) {
    auto self = reinterpret_cast<Example04*>(glfwGetWindowUserPointer(window));
#ifdef __ANDROID__
    if (!focused) {
      // On Android, surface *must* *not* be used after this function returns.
      // Destroy swapchain before destroying surface.
      self->cpool.vk.dev.swapChain.reset();
      self->instance.surface.reset(self->instance.vk);
    }
#else  /*__ANDROID__*/
    ...
#endif /*__ANDROID__*/
  }
```

This sample keeps it simple, letting go of everything right away and exiting.
It is all rebuilt again when you switch back to the app.

[Sample 5](../05indexbuffer/README.md) shows how to call `glfwIsPaused()`
(this function exists only on Android). That is all your app must do to
properly handle the Android App lifecycle. Samples 6+ use a single class,
[UniformGlue](../src/uniform_glue.h), which takes care of these little
setup and teardown details.

```C++
  int paused{0};

  int windowShouldClose(GLFWwindow* window) {
    if (!glfwWindowShouldClose(window)) {
      glfwPollEvents();
      return GLFW_FALSE;
    }
#ifdef __ANDROID__
    // glfwIsPaused() is only for Android. It reports the app lifecycle.
    paused = glfwIsPaused() ? GLFW_TRUE : paused;
#endif
    if (!paused) {
      return GLFW_TRUE;
    }
    // This supports pause on all platforms. This is actually simpler than an
    // Android-specific path.
    logI("paused. Wait for unpause or exit.\n");
    glfwSetWindowShouldClose(window, GLFW_FALSE);
    while (paused) {
      glfwWaitEvents();
#ifdef __ANDROID__
      paused = glfwIsPaused() ? paused : GLFW_FALSE;
#endif
      if (glfwWindowShouldClose(window)) {
        logI("paused - got exit request.\n");
        return GLFW_TRUE;
      }
    }
    logI("unpaused.\n");
#ifdef __ANDROID__
    // Android's biggest difference is surface, swapchain were destroyed
    // when pause began in onGLFWFocus() below. Recreate them now.
    // This is still faster than exiting, then reloading the whole app.
    // glfwWindowShouldClose will cause your app will exit, unless it
    // knows to call glfwIsPaused().
    if (glfwCreateWindowSurface(instance.vk, window, instance.pAllocator,
                                &instance.surface) ||
        onResized(cpool.vk.dev.swapChainInfo.imageExtent,
                  memory::ASSUME_POOL_QINDEX)) {
      logE("unpause: create surface or swapchain failed\n");
      return GLFW_TRUE;  // Ask app to exit.
    }
#endif
    return GLFW_FALSE;
  }

 public:
  int run(GLFWwindow* window) {
    ...

    // Begin main loop.
    ...
    while (!windowShouldClose(window)) { ... }
```

### App state

Since the app *does* exit when the user switches to the home screen or any
other window, it would be terrible if all the user's work vanished.

Android apps can store important variables to a buffer and save the pointer to
it in `glfwGetAndroidApp()->savedState`. This is important because at any time,
at the drop of a hat, the OS can completely kill your app without warning.

The size of the buffer is set by your app by writing to
`glfwGetAndroidApp()->savedStateSize`. The definitions are in the
`android_native_app_glue.h` file, included in the Android NDK.

Though this sample doesn't do that, thinking about your app in these terms is
the right mindset. Think about the few variables that really store the user's
current state. Design the app so those variables are stored in a single class
or struct. Write code that saves and restores that state. This is all just
background you should be aware of.

Applying this right away, this sample shows how to:

1. Start up **fast**. The Android app should not include things a desktop
   app is assumed to include - maybe the Android app has to connect to a cloud
   server for the game AI, etc.

1. Reduce the usage of battery power. Think of ways to reduce the visual
   quality significantly - give users an option to cut down on power usage.
   One easy way is to only use half the swapChain dimensions when creating
   the swapChain; the device can scale up the image to fit the display.

1. Handle input devices. Some Android devices have a D-Pad and joystick
   controllers. Many have touch input - but some don't. Some even have a
   keyboard. A simple way to keep your problems from exploding is to use a
   whitelist. List the Android devices in the Play Store that you **do**
   want to support. The other route, where you allow your app to be installed
   on every device in the world, can lead to impossible-to-fix bug reports and
   poor reviews.

Ok, after briefly touching on the issues around Android support, let's move on.
The goal is to help you see just how *different* Android apps are from desktop
apps.

The good news is, it's not too bad to get an app that runs on both Android
and desktop.

## GLFW and Android

[Sample 1](../01glfw/README.md) shows how to make basic calls to set up GLFW
and create a window:

1. glfwSetErrorCallback()
1. glfwInit()
1. glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API)
1. glfwCreateWindow()
1. while(!glfwWindowShouldClose(window)) {
1. glfwPollEvents()

Volcano uses a
[forked version of GLFW](https://github.com/ndsol/glfwplusandroid) that adds
Android support and multitouch (desktop machines have multitouch too). This is
what makes it possible to write an app in C++ that runs on Android and desktop.

### glfwSetMultitouchEventCallback(window, cbfun)

To receive multitouch events (such as for Android), define a multitouch
callback function:

```C++
void onGLFWmultitouch(GLFWwindow* window, GLFWinputEvent* events, int eventCount, int mods)
{
}
```

Then register the callback. This sample does the registering after then app has
reached `run()`:

```C++
  glfwSetWindowUserPointer(window, this);
  glfwSetWindowSizeCallback(window, onGLFWResized);
  glfwSetWindowFocusCallback(window, onGLFWFocus);
  glfwSetCursorEnterCallback(window, onGLFWcursorEnter);
#ifdef GLFW_HAS_MULTITOUCH
  glfwSetMultitouchEventCallback(window, onGLFWmultitouch);
#else /*GLFW_HAS_MULTITOUCH*/
  glfwSetCursorPosCallback(window, onGLFWcursorPos);
  glfwSetMouseButtonCallback(window, onGLFWmouseButtons);
  //glfwSetScrollCallback(window, onGLFWscroll);  // Not used in this sample.
#endif /*GLFW_HAS_MULTITOUCH*/
  //glfwSetKeyCallback(window, handleGLFWkey);  // Not used in this sample.
  //glfwSetCharCallback(window, handleGLFWutf32Char);  // Not in this sample.

  // Begin main loop.
  unsigned frameCount = 0;
  unsigned lastPrintedFrameCount = 0;
  Timer elapsed;
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    if (elapsed.get() > 1.0) {
      logI("%d fps\n", frameCount - lastPrintedFrameCount);
      elapsed.reset();
      lastPrintedFrameCount = frameCount;
    }

    uint32_t next_image_i;
    if (acquireNextImage(frameCount, &next_image_i,
                         imageAvailableSemaphore)) {
...
```

The existing GLFW callbacks for cursor pos and buttons are redundant.
Do not use them if your app uses multitouch. This sample shows using an
#ifdef to switch between older GLFW versions without multitouch and the Volcano
version:

```C++
#ifdef GLFW_HAS_MULTITOUCH
  static void onGLFWmultitouch(GLFWwindow* window, GLFWinputEvent* events,
                               int eventCount, int /*mods*/) {
    std::vector<PointerState> states;
    auto self = reinterpret_cast<Example04*>(glfwGetWindowUserPointer(window));
    for (int i = 0; i < eventCount; i++) {
      // Filter out events that say "FINGER just went to buttons == 0" because
      // leaving a stray line on the screen, pointing at where the vanished
      // finger last was? That looks awkward.
      if (events[i].inputDevice != GLFW_INPUT_FINGER || events[i].buttons) {
        states.push_back(PointerState{.x = events[i].x, .y = events[i].y,
                                      .b = events[i].buttons});
      }
    }
    self->onPointMove(states, !self->enteredWindow);
  }
#else /*GLFW_HAS_MULTITOUCH*/
  static void onGLFWcursorPos(GLFWwindow* window, double x, double y) {
    auto self = reinterpret_cast<Example04*>(glfwGetWindowUserPointer(window));
    PointerState state{.x = x, .y = y, .b = self->prevMouseButtons};
    self->onPointMove({state}, !self->enteredWindow);
  }

  static void onGLFWmouseButtons(GLFWwindow* window, int button, int pressed,
                                 int mods) {
    auto self = reinterpret_cast<Example04*>(glfwGetWindowUserPointer(window));
    self->prevMods = mods;
    if (pressed) {
      self->prevMouseButtons |= 1 << button;
    } else {
      self->prevMouseButtons &= ~(1 << button);
    }
  }
#endif /*GLFW_HAS_MULTITOUCH*/
```

This sample uses the onPointMove() function to update a vertex buffer. It then
sends the vertices to the GPU using Vulkan.

This is the function that generates the vertex buffer (on the CPU). It draws a
line to each multitouch input. The screenshot above shows the effect.

```C++
// vertices is the host copy of the vertex buffer.
std::vector<st_04android_vert> vertices;

// maxPointers controls the size of the vertex buffer. Think of a tablet with
// multi-touch: 10 fingers is a lot. 128 should be well beyond the limit.
constexpr static int maxPointers = 128;

// This sample draws lines (2 vertices per line).
constexpr static int VERTS_PER_PRIMITIVE = 2;

// maxVerticesSize() calculates the size of a vector of vertices.
size_t maxVerticesSize() const { return maxPointers * VERTS_PER_PRIMITIVE; }

// PointerState stores the location of each finger or mouse.
struct PointerState {
  double x, y;  // Location of this pointer.
  unsigned b;  // Bit mask of buttons that are pressed.
};

// generateVertices fills the vertices vector.
int generateVertices(const std::vector<PointerState>& states) {
  vertices.clear();
  for (const auto& state : states) {
    float x = state.x * 2 / cpool.dev.swapChainInfo.imageExtent.width - 1;
    float y = state.y * 2 / cpool.dev.swapChainInfo.imageExtent.height - 1;
    vertices.push_back(st_04android_vert{{0, 0}, {1, 1, 1}});
    vertices.push_back(st_04android_vert{
        {x, y},
        {(state.b & 1) ? 1.f : 0.f, (state.b & 2) ? 1.f : 0.f,
         (state.b & 4) ? 1.f : 0.f}});
    if (vertices.size() >= maxVerticesSize()) {
      break;  // Just in case there are too many input pointers.
    }
  }

  // Must always produce the max number of vertices. "Degenerate" vertices
  // are chosen to never be drawn. Otherwise the pipeline would have to be
  // rebuilt with a CommandBuffer specifying the number of vertices each time.
  // There is CommandBuffer:drawIndirect() to specify the number of vertices
  // in a buffer.
  while (vertices.size() < maxVerticesSize()) {
    vertices.push_back(st_04android_vert{{-2, -2}, {1, 0, 0}});
    vertices.push_back(st_04android_vert{{-2, -2}, {0, 1, 0}});
  }

  // Copy the host buffer to the stagingBuffer, then to the vertexBuffer.
  return stagingBuffer.copyFromHost(vertices) ||
         vertexBuffer.copy(cpool, stagingBuffer);
}
```

## How To Set Up a Vertex Buffer

A vertex buffer is just a plain memory::Buffer object. Somewhere in that
buffer, your app writes an array of structs containing the vertex data. This
sample shows how to use a **staging** buffer.

A staging buffer (`class Stage`) is "host visible," meaning the CPU can read or
write to it. Then a Vulkan command is used to quickly copy the contents to
the GPU.

A Vulkan command must be used because the GPU's memory is usually not
"host visible." This lets the GPU have complete freedom with its internal
memory. The GPU can run at top speed and save power that way.

Android devices often have "host visible" GPU memory anyway, so `class Stage`
automatically skips the extra copy operation if it can.

```C++
  if (cpool.ctorError() || imageAvailableSemaphore.ctorError() ||
      renderDoneFence.ctorError() || renderSemaphore.ctorError() ||
      vertexBuffer.ctorAndBindDeviceLocal() ||
      pipe0.addVertexInput<st_04android_vert>() ||
      // Read compiled-in shaders from app into Vulkan.
      vert->loadSPV(spv_04android_vert, sizeof(spv_04android_vert)) ||
      frag->loadSPV(spv_04android_frag, sizeof(spv_04android_frag)) ||
      // Add shaders to ShaderLibrary, RenderPass, and pipe0.
      shaders.add(pipe0, vert) || shaders.add(pipe0, frag) ||
      // Note: DescriptorSet is not needed since all inputs to the shaders are
      // fixed-function. This sample just uses a vertex buffer:
      generateVertices(std::vector<PointerState>{}) ||
      stage.copy(vertexBuffer, 0 /*offset*/, vertices) ||
      // Manually resize language::Framebuf the first time.
      onResized(dev.swapChainInfo.imageExtent, memory::ASSUME_POOL_QINDEX)) {
    ... handle errors ...
  }
```


<details>
<summary><b>Q:</b> What is <code>ctorError</code>? [Click to expand]</summary>

There is a
[longer explanation](https://github.com/ndsol/volcano/blob/master/CONTRIBUTING.md#api-design-guide),
but briefly:

* C++ constructors have only one way to indicate an error: `throw`.
* Vulkan constructors return an error code (`VkResult`). it is not even
  exceptional, but normal for Vulkan functions to return errors.
* It is similar to the `build()` method in a builder pattern.

Throughout Volcano, most objects are:

1. Constructed with a very small C++ constructor to initialize some
   reasonable default values.
1. Configured by your app. Your app sets members and calls methods, instead of
   passing a long list of arguments to the constructor.

   This is easier to read: it uses named fields and methods instead of
   position-dependent argument lists.
1. Built and finalized with `ctorError`. The object's members no longer affect
   Vulkan after `ctorError`, because Vulkan copied all data into the opaque
   handle, including strings and pointers to other structures (like `pNext`).

   The members and methods are kept around because it is quite normal to
   destroy and recreate the object multiple times during the execution of the
   app. They might also aid with debugging.

Thus the name `ctorError` means it is the *constructor* (abbreviated "ctor")
which finalizes the real Vulkan handle for your app. It also reports any
*error* by returning 1. "ctor" + "error" = "ctorError".

Remember the [Sample 1](../01glfw/README.md) discussion around
`CommandPoolContainer::acquireNextImage()`. Though it takes care of some logic
for you, you still need to check for the "bail out" condition. This is the
reason Volcano *obsessively* *checks* *for* *errors*, even in constructors.

--------

</details><details>
<summary><b>Q:</b> What are <code>language</code>, <code>command</code>,
<code>memory</code>, and <code>science</code>? [Click to expand]</summary>

**Short Answer:** Each is a layer of Volcano:

1. Core (in
   [`src/core/core.h`](https://github.com/ndsol/volcano/blob/master/src/core/core.h))
   (The "zeroth" layer.)
1. Language (in
   [`src/language/language.h`](https://github.com/ndsol/volcano/blob/master/src/language/language.h))
1. Command (in
   [`src/command/command.h`](https://github.com/ndsol/volcano/blob/master/src/command/command.h))
1. Memory (in
   [`src/memory/memory.h`](https://github.com/ndsol/volcano/blob/master/src/memory/memory.h))
1. Science (in
   [`src/science/science.h`](https://github.com/ndsol/volcano/blob/master/src/science/science.h))

Core has some essential libraries.

Language adds just the Vulkan types to create a logical Device.

Command adds just the Vulkan types to create a Command Buffer.

Memory adds just the Vulkan types to create an Image.

Science adds Volcano-specific shortcuts: reflection, builders and image
samplers.

**Long-ish Answer:** The
[`science::CommandPoolContainer`](https://github.com/ndsol/volcano/blob/master/src/science/science.h)
class was just introduced. `CommandPoolContainer` ties together two important
objects:

* [`command::CommandPool cpool`](https://github.com/ndsol/volcano/blob/master/src/command/command.h)
* [`command::RenderPass pass`](https://github.com/ndsol/volcano/blob/master/src/command/command.h)

`CommandPoolContainer` is in the `science` namespace, and builds on top of the
lower layers.

`RenderPass` and `CommandPool` are in the `command` namespace.

One thing Volcano does that, ideally, makes the Vulkan API easier to
understand is grouping related classes together.

The Vulkan spec does a similar thing by grouping related information into
chapters.

Each Volcano layer adds some classes by creating a new namespace.

<br/>

1. <img src="../src/fuchs-salute.png" width="200" height="212" align="right"
       alt="Vulkan Language" />
   The [`language`](https://github.com/ndsol/volcano/blob/master/src/language/language.h)
   namespace (in src/language) is for -

   Booting an **Instance** to get a logical **Device**. A **Device** provides
   a **Framebuf** which presents using an **ImageView**.

<br/>

2. The [`command`](https://github.com/ndsol/volcano/blob/master/src/command/command.h)
   namespace (in src/command) is for -

   Creating a **CommandBuffer** using a **CommandPool** to send commands to a
   device.

   **Semaphore**, **Fence**, and **Event** define the execution order of
   commands.

   Some trivial commands like data transfer require nothing else.

   But graphics commands require a **RenderPass** and **Pipeline**.
   A **Pipeline** executes code using a **Shader**.

<br/>

3. The [`memory`](https://github.com/ndsol/volcano/blob/master/src/memory/memory.h)
   namespace (in src/memory) is for -

   Allocating an **Image** or a **Buffer**.

   A **DescriptorSet** binds things to a shader for use in a pipeline. To
   allocate a DescriptorSet, use a **DescriptorPool** and a
   **DescriptorSetLayout**.

<br/>

4. <img src="../src/dna.jpg" width="190" height="190" align="right"
       alt="Vulkan Science" />
   The [`science`](https://github.com/ndsol/volcano/blob/master/src/science/science.h)
   namespace (in src/science) is for -

   * Reflection
   * Convenient shortcuts like `CommandPoolContainer`
   * The **Sampler** class defines the use of a `VkSampler` using an Image
     and ImageView.

--------

</details>

### Vulkan Memory Allocator

The difference between host coherent memory, host visible memory, and GPU-only
memory becomes important now. The
[Vulkan Memory Allocator](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/)
docs explain it well, under the section "Choosing memory type."

Volcano automatically includes
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)
(and can optionally be told to run without it). Every time you create a Buffer,
Image, or other objects, you are using Vulkan Memory Allocator.

You may find it useful to use the JSON dump feature, which lets you visually
inspect the memory usage of your app. Render the dump to an image with
[VMA Dump Vis](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/master/tools/VmaDumpVis/README.md)

[Sample 7](../07mipmaps/README.md#external-libraries)
has more information on Vulkan Memory Allocator.

## Shader Reflection Preview

This sample uses Shader Reflection, discussed in more detail in
[Sample 5](../05indexbuffer/README.md). If the following lines seem confusing, or if you
are looking for `VkVertexInputBindingDescription` or
`VkVertexInputAttributeDescription`, head over to [Sample 5](../05indexbuffer/README.md)
for the explanation.

```C++
#include "04android/struct_04android.vert.h"
...
// vertices is the host copy of the vertex buffer.
std::vector<st_04android_vert> vertices;
...
pipe0.addVertexInput<st_04android_vert>()
```

## Line Drawing (Vulkan State Changes)

Each `command::Pipeline` has a
[`topology`](https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/VkPrimitiveTopology.html)
member which can be one of:

```C++
typedef enum VkPrimitiveTopology {
    VK_PRIMITIVE_TOPOLOGY_POINT_LIST = 0,
    VK_PRIMITIVE_TOPOLOGY_LINE_LIST = 1,
    VK_PRIMITIVE_TOPOLOGY_LINE_STRIP = 2,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP = 4,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN = 5,
    VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY = 6,
    VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY = 7,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY = 8,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY = 9,
    VK_PRIMITIVE_TOPOLOGY_PATCH_LIST = 10,
} VkPrimitiveTopology;
```

This app shows how to use a **Line List** topology:
`pipe0->pipeline.info.asci.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;`

[Sample 1](../01glfw/README.md) used the default by leaving topology set to
`VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`, and rendered a single triangle.

This sample renders line segments, defined by 2 vertices per primitive.
The definition `VERTS_PER_PRIMITIVE = 2`, `maxVerticesSize()` and
`generateVertices()` generate line segment geometry. Study `generateVertices()`
to understand how the vertex buffer is put together.

[Sample 6: Three Pipelines](../06threepipelines/README.md) has multiple render
passes. A single render pass uses exactly one pipeline state object
(`command::Pipeline`) for the entire render pass. To change the topology
setting, a different pipeline state object would have to be used. That is what
[Sample 6](../06threepipelines/README.md) shows - though it sets
`VK_POLYGON_MODE_LINE` instead of `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`. See if
you can find where that constant is used in Sample 6. Regardless, changing the
pipeline state can only be done by starting another render pass.

Vulkan treats state changes as expensive. OpenGL allows draw calls and state
changes to be mixed in any order. This can be very difficult for a modern GPU
driver to optimize. (It may seem pleasant when developing an app, but poor
performance quickly changes that.)

Vulkan builds a render pass using one pipeline state ahead of time. The state
is locked in. Any draw calls then must stick to the state that was set up or
switch to a new render pass with a new pipeline state. There are exceptions,
though. There are a few state variables that Vulkan allows to be "dynamic":

* VK_DYNAMIC_STATE_VIEWPORT
* VK_DYNAMIC_STATE_SCISSOR
* VK_DYNAMIC_STATE_LINE_WIDTH

Building pipelines and render passes are a major part of Vulkan, since all
state changes are done that way. When your app exits and restarts, for
instance, the app rebuilds its render pipelines and render passes. In OpenGL
and OpenGL ES the driver would try to do that "behind the scenes" when your
app tried to render frames to the display. It is a great improvement to only
do this once, and do it under your control than to hope OpenGL did everything
the way you wanted.

## Logging

This sample ditches the `printf()` calls from [Sample 1](../01glfw/README.md)
in favor of some logging functions Volcano provides:

1. **logV:** verbose logging. Stuff almost nobody cares about.
1. **logD:** debug logging. Dumps of variable state, low priority stuff.
1. **logI:** information. Minor updates like the FPS count.
1. **logW:** warnings. The program can survive it but it isn't good.
1. **logE:** errors. The program will probably have to shut down.
1. **logF:** fatal errors. Calling `logF()` will never return; the app exits.

These functions will output to the Android Log, available via `adb logcat` or
in Android Studio.

On Windows, they log to a file `volcano.log` in the current directory and to
the debug window in Visual Studio.

On Posix systems, they log to stderr.

See [Sample 11](../11hdr/README.md) for a sample that captures the application
logs to a log window. (System logs aren't considered though.)

## The End

There are many, many more steps beyond a first time `.apk` file to get to a
professional app. This sample focuses on the steps to run the app successfully
on both Android and desktop.

<details>
<summary><b>Q:</b> How do I compile shaders with <code>shaderc</code>?
[Click to expand]</summary>

**Answer:** `shaderc` is only available for Android and not for desktop. An
Android-specific step to call `shaderc` is not a good fit for this sample,
because it is an advanced technique.

</details><details>
<summary><b>Q:</b> Can I put my shaders in the <code>res</code> directory?
[Click to expand]</summary>

**Answer:** The `res` directory is only available for Android. Volcano has a
[`androidResource(target_name)`](../06threepipelines/BUILD.gn) rule which may
help you use it to do this, but there is an even simpler solution which keeps
your app source code completely generic and cross-platform.

**Solution:**
[Publish multiple APKs to the play store](https://developer.android.com/google/play/filters.html)
and only support one CPU architecture in each APK. Use the `-target` option
when running `build-android.py`:

> `./build-android.py -t arm64`

You can continue to compile the shader into the app using
`#include` without the .APK file becoming excessively large.

</details><details>
<summary><b>Q:</b> How many Android devices support Vulkan at this point?
[Click to expand]</summary>

**Answer:** Sascha Willems has provided a
[list of Android devices that report Vulkan Support](http://vulkan.gpuinfo.org/vulkansupport.php#android_devices)

There is
[no publicly documented way](https://www.reddit.com/r/vulkan/comments/6twq0z/android_manifest_vulkan_support_filter/)
to have the app store limit an app to only Vulkan support. Please bug Google
about this!

</details><details>
<summary><b>Q:</b> Why doesn't <code>build-android.py</code> work on Windows?
[Click to expand]</summary>

**Answer:** This is a limitation of Volcano. We're working on it!

</details>

Copyright (c) 2017-2018 the Volcano Authors. All rights reserved.
