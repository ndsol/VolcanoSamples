/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 * API to grab a full screen screenshot into a VkImage
 */

#pragma once

#include <src/science/science.h>

// screenshotInit will call out.ctorError after setting its extent.
// Returns an opaque pointer to data needed for screenshotGrab, or
// returns NULL on error.
//
// NOTE: If out was already set up with a different size, it will be destroyed
//       and recreated. But if its size is correct, this is a no-op.
//       out is created in device local memory.
// NOTE: If the monitor to be shot has changed, your app must call
//       screenshotFree/screenshotInit again.
void* screenshotInit(GLFWmonitor* monitor, memory::Image& out);

// screenshotGrab overwrites the pixels in 'out'. Returns 0 if ok, 1 if error.
//
// opaque must be the pointer returned from screenshotInit.
int screenshotGrab(void* opaque, memory::Image& out,
                   command::CommandBuffer& cmd);

// screenshotFree releases the opaque pointer. This does not touch the
// memory::Image.
void screenshotFree(void* opaque);
