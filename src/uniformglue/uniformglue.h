/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 *
 * UniformGlue wraps a bunch of boilerplate:
 * * science::ShaderLibrary shaders{app.cpool.vk.dev};
 * * science::DescriptorLibrary descriptorLibrary{app.cpool.vk.dev};
 * * std::vector<std::shared_ptr<memory::DescriptorSet>> descriptorSet;
 * * std::vector<memory::Buffer> uniform;
 * * memory::Stage stage{app.cpool, memory::ASSUME_POOL_QINDEX};
 * * memory::Buffer vertexBuffer{app.cpool.vk.dev};
 * * memory::Buffer indexBuffer{app.cpool.vk.dev};
 * * std::vector<uint32_t> indices;
 * * command::Semaphore imageAvailableSemaphore{app.cpool.vk.dev};
 * * std::vector<science::SmartCommandBuffer> cmdBuffers;
 * * uint32_t frameNumber;
 * * bool paused;
 * * Timer elapsed;
 * * GLFWwindow* window;
 *
 * To use this, create a UniformGlue instance. Then use the members of
 * UniformGlue. UniformGlue is just a covenient struct to define a bunch of
 * useful objects for your app all at once.
 */

#include <src/science/science.h>

#include <algorithm>
#include <functional>
#include <map>

#include "../base_application.h"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/mat2x2.hpp>
#include <glm/vec2.hpp>

#pragma once

struct ImDrawData;
struct ImGuiContext;

#ifndef GLFW_HAS_MULTITOUCH
// Define struct here if the GLFW version is too old.
typedef struct GLFWinputEvent {
  int inputDevice;
  unsigned int num;
  unsigned int buttons;
  unsigned int hover;
  double x, y;
  double xoffset, yoffset;
  double dx, dy;
  int action;
  unsigned actionButton;
} GLFWinputEvent;

enum {
  GLFW_INPUT_UNDEFINED = 0,
  GLFW_INPUT_FIXED = 1,   // Fixed input device like a mouse.
  GLFW_INPUT_STYLUS = 2,  // Semi-fixed input device, acts a lot like a mouse.
  GLFW_INPUT_ERASER = 3,  // Eraser is similar to a stylus.
  GLFW_INPUT_FINGER = 4,  // Fingers are nothing like a mouse.
};

#define GLFW_SCROLL (GLFW_REPEAT + 1)
#define GLFW_HOVER (GLFW_SCROLL + 1)
#define GLFW_CURSORPOS (GLFW_HOVER + 1)
#endif /*GLFW_HAS_MULTITOUCH*/

// Forward declaration of ImFontConfig so #include "imgui.h" is not needed.
struct ImFontConfig;

class UniformGlue {
 protected:
  BaseApplication& app;
  const size_t maxLayoutIndex;
  const size_t imGuiLayoutIndex;
  const size_t imGuiDSetIndex;
  const unsigned uboBindingIndex;
  const size_t uboSize;
  std::pair<science::CommandPoolContainer::resizeFramebufCallback, void*>
      insertedResizeFn;
  size_t perFrameUboSize{0};

 public:
  // maxLayoutIndex: if UniformGlue wants to create a new layout (for ImGui),
  // it will use the layout index maxLayoutIndex + 1. maxLayoutIndex is also
  // assumed to be the layout containing the uniform buffer.
  UniformGlue(BaseApplication& app, GLFWwindow* window, size_t maxLayoutIndex,
              unsigned uboBindingIndex, size_t uboSize);
  virtual ~UniformGlue();

  science::ShaderLibrary shaders{app.cpool.vk.dev};
  science::DescriptorLibrary descriptorLibrary{app.cpool.vk.dev};
  std::vector<std::shared_ptr<memory::DescriptorSet>> descriptorSet;
  std::vector<memory::Buffer> uniform;
  VkBufferUsageFlags uniformUsageBits{VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
  memory::Stage stage{app.cpool, memory::ASSUME_POOL_QINDEX};
  memory::Buffer vertexBuffer{app.cpool.vk.dev};
  memory::Buffer indexBuffer{app.cpool.vk.dev};
  std::vector<uint32_t> indices;
  command::Semaphore imageAvailableSemaphore{app.cpool.vk.dev};
  command::Semaphore renderSemaphore{app.cpool.vk.dev};
  VkQueue presentQueue{VK_NULL_HANDLE};
  command::Fence renderDoneFence{app.cpool.vk.dev};
  std::vector<science::SmartCommandBuffer> cmdBuffers;
  // frameNumber is incremented each time submit() successfully returns.
  // frameNumber is passed to app.acquireNextImage() for vulkanmemoryallocator.
  // FIXME: ImGui also provides a GetFrameCount() method which does this too.
  uint32_t frameNumber{0};
  // needRebuild is set if command buffers need to be rebuilt. acquire()
  // will call app.onResized(). If onGLFWResized() is called, it can clear this
  // flag as well.
  bool needRebuild{false};

  // Your app can set paused to true to prevent windowShouldClose() from
  // returning. Yes, then your app needs to set paused to false somehow -
  // glfw events are still fired from inside windowShouldClose.
  //
  // This is the mechanism used when the Android OS pauses the app as well.
  bool paused{false};

  // elapsed can be used to measure time since the app started.
  Timer elapsed;

  // window is the GLFWwindow object. Hopefully your app no longer cares about
  // the window object once it has a UniformGlue object to handle things.
  GLFWwindow* window;

  // imGuiInit installs ImGui in the render pipeline. Your app must call this
  // before ImGui::NewFrame. If your app *might* use ImGui, your app should use
  // these convenience methods as well:
  //   * buildPassAndTriggerResize - instead of onResized
  //   * endRenderPass - instead of cmdBuffer.endRenderPass(), cmdBuffer.end()
  // They also have ImGui hooks needed to get it all working.
  WARN_UNUSED_RESULT int imGuiInit();

  // imGui* objects will not be allocated unless your app uses Dear ImGui.
  // If you do use Dear ImGui, you must call imGuiInit() before
  // ImGui::NewFrame or your app will abort with:
  // "No current context. Did you call ImGui::CreateContext() or
  //  ImGui::SetCurrentContext()?"
  bool imGuiWanted{false};
  ImGuiContext* imguiContext{nullptr};
  memory::Buffer imGuiBuf{app.cpool.vk.dev};
  static const VkDeviceSize sizeOfOneIndir =
      (sizeof(VkDrawIndexedIndirectCommand) + 3) & (~3);
  static const VkDeviceSize sizeOfIndir = sizeOfOneIndir * 4;
  void* imGuiBufMmap{nullptr};
  std::shared_ptr<science::PipeBuilder> imGuiPipe;
  science::Sampler imGuiFontSampler{app.cpool.vk.dev};
  std::shared_ptr<memory::DescriptorSet> imGuiDSet;
  size_t imGuiMaxBufUse{0};
  size_t imGuiBufWant{8192};  // Desired buffer size in arbitrary units.
  double imguiScrollY{0.0f};
  float imGuiTimestamp{0.0f};
  // fonts is guaranteed to have one shared_ptr<ImFontConfig>.
  std::vector<std::shared_ptr<ImFontConfig>> fonts;

  // ImGui captures the gamepad (sometimes), which means there is gamepad
  // mapping code in UniformGlue. Use these if your app just needs one axis.
  float curJoyX{0};
  float curJoyY{0};

  bool isImguiAvailable() const { return !!imguiContext; }

  // buildPassAndTriggerResize conveniently calls:
  //   internalImguiInit()
  //   shaders.finalizeDescriptorLibrary(descriptorLibrary)
  //   and app.onResized().
  // The first one is necessary if your app uses ImGui, to ensure imGui's pipe
  // is added last to the render pass. But it simplifies your app's code even
  // if you are not using ImGui.
  WARN_UNUSED_RESULT int buildPassAndTriggerResize();

  // endRenderPass automatically calls cmdBuf.endRenderPass() and cmdBuf.end()
  // for you. It conveniently also adds any commands needed for ImGui, if
  // imGuiInit() was called. This makes Imgui as light as possible, not
  // requiring major changes to your app to support it.
  //
  // Your app can of course do its own endRenderPass() / end(), but this may be
  // more convenient.
  WARN_UNUSED_RESULT int endRenderPass(command::CommandBuffer& cmdBuffer,
                                       size_t framebuf_i);

  // submit calls stage.flush(flight), vkQueueSubmit, renderSemaphore.present,
  // and flight.reset(). If flight is already null (you already did a
  // flight.reset() or no flight was created), it is not an error - this still
  // does vkQueueSubmit and renderSemaphore.present.
  //
  // If isAborted() is true coming in, submit cleans up flight without
  // submitting anything.
  //
  // It is also possible for app.renderSemaphore.present to set isAborted().
  // You would check for that after submit returns, though it may make no
  // real difference if your app restarts the main loop in either case.
  //
  // NOTE: SubmitInfo is not cleared, and things are added to it. Your app
  //       *must* *not* retain the same SubmitInfo between calls to submit().
  //       Always construct a new one, such as with a local variable.
  int submit(std::shared_ptr<memory::Flight>& flight, command::SubmitInfo& sub);

  void setMoveVertexBuf(std::string reason) { moveVertexBufReason = reason; }

  // initPipeBuilderFrom copies vertices and indices to device memory, and adds
  // the vertices as an input to the passed in PipeBuilder.
  //
  // In order to support any number of PipeBuilders, only the first call does
  // the one-time setup steps (copying vertices, indices). Subsequent calls
  // add settings to the passed PipeBuilder only.
  template <typename V>
  int initPipeBuilderFrom(science::PipeBuilder& pipeBuilder,
                          const std::vector<V>& vertices) {
    // initVertexBuffer will do nothing when called a second time.
    if (initVertexBuffer(vertices.data(),
                         sizeof(vertices[0]) * vertices.size())) {
      return 1;
    }

    auto& dynamicStates = pipeBuilder.info().dynamicStates;
    dynamicStates.emplace_back(VK_DYNAMIC_STATE_VIEWPORT);
    dynamicStates.emplace_back(VK_DYNAMIC_STATE_SCISSOR);

    if (pipeBuilder.addDepthImage({
            // These depth image formats will be tried in order:
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_FORMAT_D24_UNORM_S8_UINT,
        })) {
      logE("initPipeBuilderFrom: addDepthImage failed\n");
      return 1;
    }

    return pipeBuilder.addVertexInput<V>();
  }

  template <typename V>
  int updateVertexIndexBuffer(const std::vector<V>& vertices) {
    return updateVertexIndexBuffer(vertices.data(),
                                   sizeof(vertices[0]) * vertices.size());
  }

  // setDynamicUniformBuffer will modify the descriptor set layout and
  // descriptor set from VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER to
  // VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, so your app can later
  // call vkCmdBindDescriptorSets with a non-zero pDynamicOffsets.
  int setDynamicUniformBuffer(size_t perFrameUboSize_) {
    perFrameUboSize = perFrameUboSize_;
    // if buildPassAndTriggerResize() has already been called, it is too late.
    if (descriptorLibrary.isFinalized()) {
      logE("setDynamicUniformBuffer() must be called before\n");
      logE("    buildPassAndTriggerResize().");
      return 1;
    }
    // setI = 0, layoutI = maxLayoutIndex, bindingI = uboBindingIndex
    return shaders.addDynamic(0, maxLayoutIndex, uboBindingIndex);
  }

  void abortFrame();

  // isAborted returns true if the acquired frame has been aborted. As a quirk
  // it also returns true if onGLFWRefresh() has never been called.
  bool isAborted() const { return nextImage == (uint32_t)-1; }

  // getImage returns the swapchain image index from the last acquire(),
  // which happens at the top of onGLFWRefresh.
  uint32_t getImage() const { return nextImage; }

 protected:
  // buildFramebuf initializes cmdBuffers and the uniform buffer, since both
  // are done on a per-framebuffer basis.
  // Registered with CommandPoolContainer in UniformGlue::UniformGlue().
  int buildFramebuf(size_t framebuf_i, size_t poolQindex);

  // initVertexBuffer will do nothing when called a second time.
  int initVertexBuffer(const void* vertices, size_t verticesSize);

  // updateVertexIndexBuffer will update both vertexBuffer and indexBuffer,
  // destroying and re-allocating them to fit vertices and this->indices.
  int updateVertexIndexBuffer(const void* vertices, size_t verticesSize);

  int internalImguiInit();

  // checkImGuiBufSize is called by acquire.
  int checkImGuiBufSize(struct ImDrawData* drawData);

  // imGuiRender is called by submit. See endRenderPass() and
  // imGuiAddCommands().
  int imGuiRender(struct ImDrawData* drawData);

  // imGuiInnerRender is called by imGuiRender.
  int imGuiInnerRender(struct ImDrawData* drawData,
                       VkDrawIndexedIndirectCommand& indir);

  // imGuiAddCommands is called by endRenderPass() to add ImGui drawing
  // commands to cmdBuffer. This is not called every frame, but just once to
  // prepare the cmdBuffer with a vkCmdDrawIndexedIndirect. The indirect buffer
  // is updated every frame when submit calls imGuiRender.
  WARN_UNUSED_RESULT int imGuiAddCommands(command::CommandBuffer& cmdBuffer,
                                          size_t framebuf_i, uint32_t subpass);

  // acquire is how to get the nextImage (from vkAcquireNextImageKHR).
  // If isAborted() is true, do not call submit, rather go back to the top of
  // the main loop.
  int acquire();

  uint32_t nextImage{(uint32_t)-1};
  uint32_t fastButtons{0};
  uint32_t curFrameButtons{0};

  // inGLFWRefresh can detect recursion-related bugs.
  bool inGLFWRefresh{false};

  // stillHaveAcquiredImage is used to prevent acquire() from throwing away
  // nextImage just because the redraw callbacks went back to the top of the
  // main loop. There is no way to stuff an image back into the Vulkan queue.
  //
  // NOTE: There are Vulkan-specified paths where the main loop must go "back
  // to the top," which *discard* the acquired image. Not all abort paths will
  // set stillHaveAcquiredImage = true, just application-level aborts.
  bool stillHaveAcquiredImage{false};
  uint32_t lastAcquiredImage{(uint32_t)-1};
  std::string moveVertexBufReason;

 public:
  // windowShouldClose returns the same result as glfwWindowShouldClose(), and
  // automatically updates ImGui state at the start of the frame.
  int windowShouldClose();

  // prevInput (updated by onGLFWmultitouch) is the previous input events.
  std::vector<GLFWinputEvent> prevInput;
  // prevMods (updated by onGLFWmultitouch) has bits for each modifier key.
  int prevMods{0};
  // prevEntered is 1 when a pointer is in the window.
  int prevEntered{0};
  // focused is 1 when the window is focused. Your app can be notified when
  // focused changes by adding an inputEventListener. You can tell if the
  // event is a change to focused and/or entered if you save the previous
  // value of focused somewhere. prevEntered is saved for you, above.
  int focused{0};
  // scaleX, scaleY are for hi DPI screens that scale the UI.
  float scaleX{1.f}, scaleY{1.f};

  typedef void (*inputEventListener)(void* self, GLFWinputEvent* events,
                                     size_t eventCount, int mods, int entered);
  std::vector<std::pair<inputEventListener, void*>> inputEventListeners;

  typedef void (*keyEventListener)(void* self, int key, int scancode,
                                   int action, int mods);
  std::vector<std::pair<keyEventListener, void*>> keyEventListeners;

  typedef void (*charEventListener)(void* self, const char* utf8str);
  std::vector<std::pair<charEventListener, void*>> charEventListeners;

  // redrawListener is called after acquire() succeeds. If abortFrame() is
  // called, onGLFWRefresh will stop without setting redrawErrorCount. This is
  // for when Volcano needs to restart the main loop.
  typedef int (*redrawListener)(void* self, std::shared_ptr<memory::Flight>&);
  std::vector<std::pair<redrawListener, void*>> redrawListeners;

  // redrawErrorCount is incremented if any redrawListeners return 1.
  unsigned redrawErrorCount{0};

  // onGLFWRefresh is called by GLFW but can also be called by your app to
  // run all redrawListeners. Pass in UniformGlue::window as the first arg.
  static void onGLFWRefresh(GLFWwindow* window);

  // getImGuiRotation allows your app to use the same rotation of the screen
  // that ImGui uses.
  void getImGuiRotation(glm::mat2& rot, glm::vec2& translate);

 protected:
  void processInputEvents(GLFWinputEvent* events, int eventCount, int mods);
  int onGLFWRefreshError();
  static void onGLFWResized(GLFWwindow* window, int w, int h);
  static void onGLFWFocus(GLFWwindow* window, int focused);
#if defined(GLFW_HAS_MULTITOUCH) && !defined(VOLCANO_TEST_NO_MULTITOUCH)
  static void onGLFWmultitouch(GLFWwindow* window, GLFWinputEvent* events,
                               int eventCount, int mods);
#else  /*GLFW_HAS_MULTITOUCH*/
  static void onGLFWcursorPos(GLFWwindow* window, double x, double y);
  static void onGLFWmouseButtons(GLFWwindow* window, int button, int pressed,
                                 int mods);
  static void onGLFWscroll(GLFWwindow* window, double scroll_x,
                           double scroll_y);

  int prevMouseButtons{0};
#endif /*GLFW_HAS_MULTITOUCH*/

#ifdef __ANDROID__
  std::map<int, int> keyTooFast;
#endif /*__ANDROID__*/
  static void onGLFWcursorEnter(GLFWwindow* window, int entered);
  static void onGLFWcontentScale(GLFWwindow* window, float x, float y);
  static void onGLFWkey(GLFWwindow* window, int key, int scancode, int action,
                        int mods);
  static void onGLFWchar(GLFWwindow* window, unsigned utf32ch);
};
