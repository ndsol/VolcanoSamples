<table cellspacing="0" cellpadding="0"><tbody>
<tr valign="top"><td width="60%" colspan="2">

# Volcano Sample 3: Render To Disk

This sample uses [Volcano](https://github.com/ndsol/volcano) without using
`VK_KHR_swapchain`. The idea comes from
[Intel's Introduction to Vulkan, part 2](https://software.intel.com/en-us/articles/api-without-secrets-introduction-to-vulkan-part-2):
try "acquiring the pointer to a buffer's (texture's) memory (mapping it) and
copying data from it".

This can make your unit tests faster and have fewer dependencies by rendering
to disk (except, of course, if you need to test the window system).

</td></tr><tr valign="top"><td width="60%">

[View source code](./)

1. [Goals](#goals)
1. [Headless Mode](#headless-mode)
   1. [Removing the `PRESENT` requirement](#removing-the-present-requirement)
1. [Image Transitions](#image-transitions)
   1. [Create Images](#create-images)
   1. [Set Up For Rendering](#set-up-for-rendering)
   1. [Set Up For Copying](#set-up-for-copying)
1. [Fences Vs. Semaphores](#fences-vs-semaphores)
   1. [`command::Fence`](#commandfence)
   1. [`command::Semaphore`](#commandsemaphore)
   1. [`command::Event`](#commandevent)
   1. [`Pipeline Barriers`](#pipeline-barriers)
   1. [`Timeline Semaphores`](#timeline-semaphores)
1. [`hostImage` vs `hostBuffer`](#hostimage-vs-hostbuffer)
1. [The End](#the-end)

</td><td width="40%">

[The top level README](https://github.com/ndsol/VolcanoSamples/) shows how to
build this sample.

Run this sample by typing:<br/>`out/Debug/03rendertodisk`

The output file `test0.bmp` (a "screenshot") is saved in the current directory
where the app runs.

Enable Vulkan Validation layers by typing:<br/>
`VK_INSTANCE_LAYERS=\`<br/>
`VK_LAYER_LUNARG_standard_validation \`<br/>
`out/Debug/03rendertodisk`</td></tr></tbody></table>

## Goals

After you have studied this sample, you should be able to explain:

1. Why is `VK_KHR_swapchain` not part of the core Vulkan spec?

2. What are image transitions used for?

3. When is a `Fence` better? When is a `Semaphore` better?

## Headless Mode

In [Sample 1](../01glfw/README.md) we used a `crossPlatformMain()` function to
show how Vulkan works pretty much the same for all supported OSes.

This sample has no app window, so let's rip out GLFW and the
[`VkSurfaceKHR`](https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/VkSurfaceKHR.html)
object. This sample consists entirely of a pair of C-style functions
(and some change):

* [`headlessMain()`](03rendertodisk.cpp#L256) is the cross-platform main() that
  sets up the `language::Instance`.
* [`example03()`](03rendertodisk.cpp#L114) contains the main loop

For a sample that can run in both headless and GLFW modes, see
[Sample 20 - Texture Compression](../20compute).

#### Removing the `PRESENT` requirement

[`headlessMain`](03rendertodisk.cpp#L256) creates a custom
[`language::Instance`](https://github.com/ndsol/volcano/blob/master/src/language/language.h#L570)
that renders into an off-screen frame buffer.

The call to `instance.ctorError()` followed by `instance.open()` work like you
expect them to. But
[`language::PRESENT`](https://github.com/ndsol/volcano/blob/master/src/core/structs.h#L279)
is deleted from the `Instance::minSurfaceSupport` vector. That removes the
requirement for `instance` to look for devices with a screen - by default the
`Instance::minSurfaceSupport` set includes `PRESENT`. Removing it enables
the instance to use devices that have no screen at all:

```C++
if (!instance.minSurfaceSupport.erase(language::PRESENT)) {
  logE("removing PRESENT from minSurfaceSupport: not found\n");
  return 1;
}
if (instance.ctorError(nullptr, 0 /*extensionSize*/, emptySurfaceFn,
                       nullptr) ||
    // Set Vulkan frame buffer (image) size to 800 x 600.
    instance.open({800, 600})) {
  return 1;
}
```

Inside
[`instance.ctorError`](https://github.com/ndsol/volcano/blob/master/src/language/language.h#L600)
Volcano fills in `Device::presentModes` with the result of
`vkGetPhysicalDeviceSurfacePresentModesKHR`. Then they are all removed by
[`headlessMain`](03rendertodisk.cpp#L279). This is a hint to the instance not
to load the `VK_KHR_swapchain` extension:

```C++
// Remove auto-selected presentModes to prevent VK_KHR_swapchain being used
// and device framebuffers being auto-created.
auto& dev = *instance.devs.at(0);
dev.presentModes.clear();
```

An app that does have a window and a Vulkan device surface must choose a
framebuffer format that matches the surface. This sample is free of that
restriction and can use any format that works. A 24-bit `.BMP` file seems like
a good choice. To keep the shaders the same as [Sample 1](../01glfw/README.md)
only two formats can be counted on to work. Fortunately, they are required for
any device that supports Vulkan:

* `VK_FORMAT_R8G8B8A8_SRGB`
* `VK_FORMAT_R8G8B8A8_UINT`

Calling `Device::chooseFormat` with those two formats will automatically pick
one that the device supports (or return an error).

```C++
dev.swapChainInfo.imageFormat = dev.chooseFormat(
    VK_IMAGE_TILING_OPTIMAL,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
    {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UINT});
```

The result is stored in the `Device::swapChainInfo` member. (This sample does
not use a swapChain but the `imageFormat` member is still used.)

<details>
<summary><b>Q:</b> Why is <code>VK_KHR_swapchain</code> not part of the core
Vulkan spec? [Click to expand]</summary>

Check back in [Sample 1](../01glfw/README.md) under the section
*"Use VK_KHR_swapchain to present an image to the screen"*.

**Answer:** It is an optional extension and not part of the core specifically
for apps like this one that don't need it.
</details><details>
<summary><b>Q:</b> What is the difference between
<code>VK_FORMAT_R8G8B8A8_SRGB</code> and <code>VK_FORMAT_R8G8B8A8_UINT</code>?
[Click to expand]</summary>

**Answer:** In
[device.cpp](https://github.com/ndsol/volcano/blob/master/src/language/device.cpp),
the color space displayed on the screen comes from whatever Vulkan suggests
first:

```C++
int initSurfaceFormat(Device& dev) {
  if (dev.surfaceFormats.size() == 0) {
    logE("BUG: should not init a device with 0 SurfaceFormats\n");
    return 1;
  }

  if (dev.surfaceFormats.size() == 1 &&
      dev.surfaceFormats[0].format == VK_FORMAT_UNDEFINED) {
    // Vulkan specifies "you get to choose" by returning VK_FORMAT_UNDEFINED.
    // Default to 32-bit color and hardware SRGB color space. Your application
    // probably wants to test dev.surfaceFormats itself and choose its own
    // imageFormat.
    dev.swapChainInfo.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    dev.swapChainInfo.imageColorSpace = dev.surfaceFormats[0].colorSpace;
    return 0;
  }

  // Default to the first surfaceFormat Vulkan indicates is acceptable.
  dev.swapChainInfo.imageFormat = dev.surfaceFormats.at(0).format;
  dev.swapChainInfo.imageColorSpace = dev.surfaceFormats.at(0).colorSpace;
  return 0;
}
```

But notice that Vulkan only defines one (unless you enable extensions):
[VK_COLOR_SPACE_SRGB_NONLINEAR_KHR](https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/VkColorSpaceKHR.html).

</details>
<details>
<summary><b>Q:</b> What other color spaces are supported? [Click to expand]</summary>

`VK_EXT_swapchain_colorspace` adds EXTENDED_SRGB with LINEAR and NONLINEAR
transfer functions. It also adds DCI_P3 (LINEAR/NONLINEAR), HDR10 (BT2020),
DOLBYVISION, and ADOBERGB.

It also supports VK_COLOR_SPACE_PASS_THROUGH_EXT.

With `VK_EXT_swapchain_colorspace` enabled, `VK_AMD_display_native_hdr` adds
VK_COLOR_SPACE_DISPLAY_NATIVE_AMD which specifies the display's native color
space.

Volcano automatically chooses a format the device can present. But if your app
tells Volcano not to worry about presenting, your app must set up the format and
color space on its own.

Ok, Vulkan only defines the SRGB_NONLINEAR color space. Got it.

Wondering what SRGB_NONLINEAR is?

* VK_FORMAT_R8G8B8A8_SRGB uses a very specific formula to encode the energy of
  Red, Green, and Blue pixels
  [how human eyes perceive things](https://en.wikipedia.org/wiki/SRGB). Human
  eyes are
  [really awesome and non-linear](https://youtu.be/m9AT7H4GGrA).

* VK_FORMAT_R8G8B8A8_UINT encodes pixels using a linear function. Linear here
  refers to the amount of energy added for each unit of a pixel's value.
  If a pixel changes in value from 1 to 2, it changes "1 unit" of light
  intensity. A linear function always changes the light intensity by the same
  amount - whether the pixel changes from 1 to 2 or from 251 to 252.

* Vulkan, modern GPUs and monitors all assume a nonlinear function. This makes
  bright colors brighter and dark colors darker.
</details>

## Image Transitions

Image transitions shuffle the image's pixels so they match the varying
requirements when the device uses the image in different ways. The exact
way a layout works will be hidden inside the device driver, but an app can
tell Vulkan the image is about to be:

* Used by shaders. Transition to SHADER_READ_ONLY_OPTIMAL layout.
* Copied to/from device-local memory. Transition to TRANSFER_SRC_OPTIMAL or
  TRANSFER_DST_OPTIMAL.
* Read on the host after TRANSFER_DST_OPTIMAL. Transition to the GENERAL
  layout.
* Initialized by the host, e.g. by an image library or disk I/O. Create the
  image with the PREINITIALIZED layout, which just trashes the memory.
* Used as the frame buffer (attached to the frame buffer). The render pass
  will automatically transition the image to the
  [PRESENT_SRC_KHR](https://github.com/ndsol/volcano/blob/master/src/command/pipeline.cpp#L65)
  layout.

This sample demonstrates how to use image layout transitions.

#### Create Images

The first part of [`example03()`](03rendertodisk.cpp#L114) sets up two images.
`hostImage` is created in HOST_VISIBLE memory using LAYOUT_UNDEFINED.

```C++
  std::unique_ptr<memory::Image> hostImage(new memory::Image{dev});
  ...
  hostImage->info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  hostImage->info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (hostImage->ctorAndBindHostCoherent()) {
    ... handle error ...
  }
```

The `hostImage->ctorAndBindHostCoherent()` call does the rest to set up
hostImage to be "host coherent," meaning the memory can be read and written by
the CPU.

To get the rendered image on the CPU, first `image` is created in DEVICE_LOCAL
memory. This image is in memory that the GPU can access without involving the
CPU. In fact, the CPU can't even see the contents of a DEVICE_LOCAL image.

```C++
  if (cpool.ctorError() || renderDoneFence.ctorError() ||
      image->ctorAndBindDeviceLocal() ...
```

#### Set Up For Rendering

Vulkan does not allow `hostImage` to be created in LAYOUT_TRANSFER_DST_OPTIMAL
directly, but it can start in LAYOUT_UNDEFINED and then transition. It needs to
be DST_OPTIMAL to receive the "screenshot" from the GPU's "device local" image.

```C++
    // Copy from image to hostImage
    science::copyImage1to1(cmdBuffer, *image, *hostImage) ||
```

`image` comes already set to the correct
[LAYOUT_PREINITIALIZED by default](https://github.com/ndsol/volcano/blob/master/src/memory/memory.h#L241):
```C++
info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
```

Later, when the `beginPrimaryPass()` command is executed on the GPU, Vulkan
automatically transitions `image` to PRESENT_SRC_KHR. On the CPU, Volcano is
not aware of this change and has to be told about it:

```C++
  // pipe0->info.attach describes the framebuf.attachments.
  // cmdBuffer.beginSubpass() will transition image to a new layout (see
  // definition of VkAttachmentDescription2KHR::finalLayout).
  // image->currentLayout is just a Volcano state variable. It needs to be
  // updated manually to match what happens in the render pass:
  image->currentLayout = pipe0->info.attach.at(0).vk.finalLayout;
```

#### Set Up For Copying

The commands to begin a render pass, draw something, and then "screenshot" the
results get recorded into a `command::CommandBuffer`.
[Sample 1](../01glfw/README.md) shows how to set up a render pass and command
buffers.

Only when the CommandBuffer has finished executing on the GPU will the result
be ready for the CPU to process. This code just sets everything up.

```C++
// image will get transitioned to VK_IMAGE_LAYOUT_PRESENT_SRC_KHR by
// RenderPass but cmdBuffer.barrier() does not know it. Tell it here.
image->currentLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

command::CommandBuffer& cmdBuffer = cmdBuffers.at(0);
if (cmdBuffer.beginSimultaneousUse() ||
    cmdBuffer.beginPrimaryPass(pass, framebuf) ||
    ... use drawing commands to create something pretty ...
    // End RenderPass.
    cmdBuffer.endRenderPass() ||
```

Right after `endRenderPass()`, the code uses `science::copyImage1to1()`
to copy the rendered result to hostImage. Inside `copyImage1to1()`, the image
and hostImage are transitioned to a new layout. The image is transitioned to
SRC_OPTIMAL, since it will be the source of the copy. The hostImage is
transitioned to DST_OPTIMAL, since it will be the destination of the copy.

```C++
    // End RenderPass.
    cmdBuffer.endRenderPass() ||
    // Copy from image to hostImage
    science::copyImage1to1(cmdBuffer, *image, *hostImage) ||
```

`hostImage` is then transitioned to LAYOUT_GENERAL. That is the only layout
which allows the host to read out the data.

```C++
    cmdBuffer.barrier(*hostImage, VK_IMAGE_LAYOUT_GENERAL) ||
    cmdBuffer.end()) {
```

Even after all this work, the cmdBuffer has so far only buffered all the steps
necessary to spit out `hostImage` on the CPU. When the cmdBuffer is finally
submitted to the GPU it will do everything asynchronously:

```C++
// Submit the VkCommandBuffer to the device.
if (cmdBuffers.at(0).submit(memory::ASSUME_POOL_QINDEX, {}, {}, {},
                            renderDoneFence.vk)) {
  ... handle error ...
}
```

The cmdBuffer begins executing at this point. The host CPU now needs to wait
for the fence to know when the GPU has finished.

## Fences Vs. Semaphores

Vulkan actually has 5 different primitive types for synchronization. Volcano
wraps them in Volcano classes:

1. `VkFence` - the Volcano class is `command::Fence`.
1. `VkSemaphore` - the Volcano class is `command::Semaphore`.
1. `VkEvent` - the Volcano class is `command::Event`.
1. Pipeline Barriers - there are several Volcano methods named
   `command::CommandBuffer::barrier()`.
1. Timeline Semaphores - see below.

Though Fences, Semaphores, etc. really only halt the GPU or CPU, think of them
more like a traffic light that starts out in the "STOP" state. When a certain
amount of work has passed, they switch to the "GO" state, called the
"signalled" state.

For maximum performance, your app ideally hits the traffic light when it has
already been "signalled" and is in the "GO" state. What would happen if you
just skipped all Fences, Semaphores, etc. and just let the CPU and GPU run
wild?

Well, undefined behavior up to and including locking up the computer, but
more specifically, your code would not execute in order. Inputs to one part
of the program would be garbage because the previous part may have written some
or may not have written anything yet. (This is known as a read after write
hazard, or "RaW hazard," and yes, skipping all synchronization can lead to
other hazards such as a write after write hazard, etc.)

### `command::Fence`

A `command::Fence`can halt the CPU host until the GPU device reaches a certain
point in its execution. Only the GPU can set the fence (send the signal). Only
the CPU can reset the fence (receive the signal).

If you only want to check (poll) the Fence on the CPU, but not block, use
`Fence::getStatus()`.

<img src="vulkan-sync-types.png" width="937" height="356" border="0"
  align="center"/>

### `command::Semaphore`

A `command::Semaphore` can halt one thread on the GPU until another thread
reaches a certain point (though "thread" is not a good term to describe how
GPUs execute code).

In this sample the `renderSemaphore` is replaced with a `renderDoneFence`
because the GPU is not going to present the image using a swap chain. Instead,
the CPU is going to grab a copy and write it out as a `.BMP`.

Since there is only one `VkCommandBuffer` and no semaphores, there is no call
to `vkAcquireNextImageKHR` like in [Sample 1](../01glfw/README.md). Instead,
this sample just calls submit:

```C++
// Submit the VkCommandBuffer to the device.
if (cmdBuffers.at(0).submit(memory::ASSUME_POOL_QINDEX, {}, {}, {},
                            renderDoneFence.vk)) {
  return 1;
}
```

It is still designed to run as fast as possible - for about 1000 frames. A
single frame is all that is needed. Repeating the render loop thousands of
times can help find performance slowdowns.

### `command::Event`

`Semaphore` and `Event` types are useful for GPU - GPU synchronization,
such as between threads (use a Sempahore) or between Command Buffers (use an
Event).

The `Fence` class is useful for having the CPU wait for the GPU.

(`Event` is not supported by Metal 1.0, so use of `Event` may limit your
app's portability to macOS.)

### Pipeline Barriers

A Pipeline Barrier can halt a subpass until a previous subpass has reached
a certain point in its execution:

<img src="vulkan-pipeline-barriers.png" width="1080" height="568"
  border="0" align="center"/>

### Timeline Semaphores

This new type of semaphore can replace Fence, Semaphore and Event, and can
dramatically simplify your app logic.

This requires the `VK_KHR_timeline_semaphore` extension. See
[vkSignalSemaphoreKHR](https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/vkSignalSemaphoreKHR.html)

## `hostImage` vs `hostBuffer`

This sample creates a `hostImage` with a very particular layout so the call to
`hostImage->mem.mmap()` will succeed.

This is actually the hard way to get an image from the GPU. The "shortcut" is
found by carefully reading the VkBuffer spec:

https://www.khronos.org/registry/vulkan/specs/1.0/man/html/VkBuffer.html

"Buffers represent linear arrays of data"

When you begin thinking of transitioning an image to "host visible" and
"TILING_LINEAR," substitute a buffer. **Buffers are TILING_LINEAR by
definition.** Just use a buffer for the image data and then copy out of
or into an image that is DEVICE_LOCAL. The copy operation will automatically
put the data in a linear format because buffers are TILING_LINEAR by
definition.

Take a look at `CommandBuffer::copyBufferToImage()` and `copyImageToBuffer()`.
You may have to transition the GPU image to TRANSFER_SRC_OPTIMAL or
TRANSFER_DST_OPTIMAL of course.

Using a buffer instead of an image lets you:

1. Copy any mip level or access an image array.
   (If you used an image, you would not be able to access mip levels or image
   arrays. TILING_LINEAR images do not support multiple mip levels or image
   arrays.)

1. Reuse the same buffer to "stage" other types of data without
   destroying and recreating it. For example, a single buffer may be used to
   upload a texture and then vertex data immediately after. A buffer can be
   reused since it is just an untyped block of memory.

1. Copy data in smaller chunks. Rather than allocate 2x the required memory
   (one copy in HOST_VISIBLE memory and one in DEVICE_LOCAL memory), you can
   design your code to start a GPU transfer as soon as a certain fixed amount
   of data is ready. This is a more advanced technique demonstrated in
   [Sample 10](../10cubemap) and [Sample 11](../11hdr).

## The End

Hopefully this sample has helped clarify:

1. What `VK_KHR_swapchain` is, when it is needed, and when you can skip it.
1. How to write a Volcano app that runs "headless" (with no window or display).
1. Image layouts, image transitions, and image barriers.
1. When to use a buffer instead of an image.

#### Homework

Why does this sample use a Fence?

Why does it **not** use any Semaphores?

Why does this sample use a CommandBuffer?

Why does it **not** use a RenderPass?

The [next sample](../04android/README.md) adds Android support, user input
handling, and several other interesting details.

Synchronization images Copyright (c) 2019 Khronos Group. Licensed under
CC BY 4.0.

Copyright (c) 2017-2018 the Volcano Authors. All rights reserved.
