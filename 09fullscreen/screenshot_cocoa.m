#include <src/science/science-glfw.h>
#include <MoltenVK/API/vk_mvk_moltenvk.h>

#import <ApplicationServices/ApplicationServices.h>
#import <QuartzCore/QuartzCore.h>

int _platformScreenshotCocoa(GLFWmonitor* monitor, VkExtent3D ex, VkImage out,
                             void* vkGetMTLTextureMVK)
{
  logW("https://github.com/KhronosGroup/Vulkan-Loader/issues/146\n");
  logW("This needs SDK 1.1.106 or later to activate VK_GFX_metal\n");
  logW("currently the VkImage handle cannot be used directly by MoltenVK\n");
  logW("because it is wrapped\n");
  if (@available(macOS 10.13, *)) {
    // Fetch the underlying MTLTexture from out.
    id<MTLTexture> tex;
    ((PFN_vkGetMTLTextureMVK)vkGetMTLTextureMVK)(out, &tex);

    // Do the actual screenshot
    CGImageRef imageRef = CGDisplayCreateImage(glfwGetCocoaMonitor(monitor));
    NSUInteger width = CGImageGetWidth(imageRef);
    NSUInteger height = CGImageGetHeight(imageRef);
    if (width != ex.width || height != ex.height) {
      logE("screenshot got %u x %u, want %u x %u\n", (unsigned)width,
           (unsigned)height, ex.width, ex.height);
      CGImageRelease(imageRef);
      return 1;
    }

    CALayer* layer = [[CALayer layer] retain];
    layer.contents = (__bridge id) imageRef;

    // CARenderer (Core Animation) to copy the contents of imageRef to tex
    CARenderer* pass = [[CARenderer rendererWithMTLTexture:tex options:nil] retain];
    // What to render: layer with layer.contents = imageRef
    pass.layer = layer;
    pass.bounds = CGRectMake(0, 0, width, height);

    [pass beginFrameAtTime: CACurrentMediaTime() timeStamp: nil];
    [pass addUpdateRect: pass.bounds];
    [pass render];
    [pass endFrame];

    [pass release];
    [layer release];
    CGImageRelease(imageRef);
    return 0;
  } else {
    logE("screenshot direct to metal requires macOS 10.13\n");
    return 1;
  }
}
