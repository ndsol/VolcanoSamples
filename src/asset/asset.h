/* Copyright (c) 2017-2018 the Volcano Authors. Licensed under the GPLv3.
 */

#include <src/command/command.h>
#include <src/memory/memory.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <forward_list>
#include <memory>
#include <set>
#include <vector>

#include "../uniformglue/uniformglue.h"

#pragma once

namespace asset {

#if 1
typedef uint32_t indicesType;
static constexpr VkIndexType indexBufVkIndexType = VK_INDEX_TYPE_UINT32;
#else
typedef uint16_t indicesType;
static constexpr VkIndexType indexBufVkIndexType = VK_INDEX_TYPE_UINT16;
#endif

// Vertex defines the per-vertex data all assets must provide.
typedef struct Vertex {
  glm::vec3 P{0.f, 0.f, 0.f};  // position
  glm::vec3 N{0.f, 0.f, 0.f};  // normal
  glm::vec2 uv{0.f, 0.f};      // texture coordinates
  glm::vec4 color{0.f, 0.f, 0.f, 1.f};
  // custom can be a convenient place to store more texture coordinates,
  // material data, etc. in your VertexEvalFn.
  glm::vec4 custom{0.f, 0.f, 0.f, 0.f};

  inline void add(const Vertex& other) {
    N += other.N;
    uv += other.uv;
    color += other.color;
  }
  inline void divideByScalar(float a) {
    N /= a;
    uv /= a;
    color /= a;
  }
} Vertex;

// VertexIndex stores a collection of vertices with their draw order 'order'.
typedef struct VertexIndex {
  std::vector<Vertex> vert;
  std::vector<indicesType> order;
} VertexIndex;

// Track allocations in the vertex and index buffer.
typedef struct FreeBlock {
  size_t base{0};
  size_t use{0};
} FreeBlock;

enum {
  // A vertex defines 2+ faces, if the faces have the same normal, shading can
  // make the edge look "smooth." But if different, the edge looks "sharp."
  // Set flags |= VERT_PER_FACE in VertexEvalFn to define different
  // normals.
  VERT_PER_FACE = 1,
};

// VertexEvalFn is called for each face in the asset.
// WARNING: VertexEvalFn can also *not* be called, or be called in any order.
// If BaseAsset::eval is nullptr, Vertex has reasonable defaults and
// VERT_PER_FACE is assumed to be 0.
// If your VertexEvalFn does not set flags |= VERT_PER_FACE, then the values of
// Vertex::N and color are averaged between all faces that touch the Vertex,
// and only a single Vertex is stored. This may save memory. It also makes the
// edge look "smooth." Note it shares *all* Vertex data, like uv and custom!
//
// NOTE: Your app gets the final say on how vertex data translates to inputs
// for your vertex shader in Library::write.
typedef int (*VertexEvalFn)(void* userData, std::vector<Vertex>& vert,
                            uint32_t& flags);

// VertexWriteFn is called to copy the contents of vert to your shader's
// vertex shader. This makes the code cleaner than if there were heavy use of
// template types and requirements of the name of your shaders' inputs.
// NOTE: 'dst' points into the Library::vertexBuf where the vertex is to
// be written - Library can do partial updates of vertexBuf.
typedef int (*VertexWriteFn)(void* userData, const Vertex& vert, void* dst);

// BaseAsset::state() values
enum BaseAssetState {
  INVALID = 0,
  READY,     // The asset is ready on the GPU.
  DELETED,   // The asset is on the GPU, but not the CPU. Delete it.
  DEL_WAIT,  // The asset is partially on the GPU. Wait.
  ADDED,     // The asset is on the CPU, but not the GPU. Add it.
  ADD_WAIT,  // The asset is partially on the GPU. Wait.
};

// BaseAsset defines what an asset can do: it can output its vertex and index
// data, you can pass its VkDrawIndexedIndirectCommand to a command buffer.
typedef struct BaseAsset {
  science::InstanceBuf inst;

  std::pair<VertexEvalFn, void*> eval{nullptr, nullptr};

  // toVertices validates the asset, calls eval and writes the result to 'out'.
  // toVertices always draws from vertex 0 - the inst.cmd.vertexOffset is used
  // to offset the indices in the larger Library::vertexBuf.
  virtual int toVertices(VertexIndex& out) = 0;

  // doTri is a helper method for the shapes to call eval.
  int doTri(indicesType i0, indicesType i1, indicesType i2,
            std::vector<Vertex>& raw, std::vector<indicesType>& shared,
            VertexIndex& out, uint32_t flags);

  // doShared is a helper method for the shapes to average normals.
  int doShared(std::vector<Vertex>& raw, std::vector<indicesType>& shared,
               VertexIndex& out);

  BaseAssetState state() const { return state_; }
  size_t verts() const { return vblk.use; }
  size_t indices() const { return oblk.use; }

 protected:
  friend class Library;
  // state is used by the Library to handle in-flight assets after Library::del
  // and assets after Library::add. To know when your asset can actually
  // be drawn (when you can set inst.cmd.instanceCount > 0), wait until
  // state() == asset::READY.
  BaseAssetState state_{INVALID};
  // block tracks where this is in the vertex and index buffer, updated by
  // Library::write().
  FreeBlock vblk, oblk;
  // frameNumber is used by Library.
  uint32_t frameNumber{0};
} BaseAsset;

// Revolv creates the volume by revolving the line segments in 'pt' around the
// origin, starting at 0 deg and sampling 'rots' times total around the +Y axis
// A single "slice" can be made by setting rotStart to something other than 0.
// NOTE: This does not do auto LOD, bezier curves - meant to be self-contained.
// WARNING: 'pt' is not validated. Beware non-convex or self-intersecting lines
typedef struct Revolv : public BaseAsset {
  // rots is the number of times the sweep is sampled. Minimum of 3.
  // rotStart starts the sampling at a value other than 0.
  // But keep rotStart <= rots - 3.
  uint32_t rots{3}, rotStart{0};
  // aspectZ is the Z axis factor to turn a circle into an ellipse. Min of 0.
  float aspectZ{1.0f};
  // pt defines the 2D line segments revolved around the origin. Minimum of 2.
  // In order to maintain counter-clockwise order, the y-values must be in
  // ascending order (bottom to top).
  std::vector<glm::vec2> pt;

  enum {
    DELETE_CAP_T = 1,  // Deletes "top," formed from revolving the last pt
    DELETE_CAP_B = 2,  // Deletes "bottom," formed from revolving pt[0]
    DELETE_CAP_L = 4,  // Left and right caps not implemented yet.
    DELETE_CAP_R = 8,  // Left and right caps not implemented yet.
  };
  uint32_t flags{0};

  // toVertices validates the asset, calls eval and writes the result to 'out'.
  virtual int toVertices(VertexIndex& out);
} Revolv;

// Extrud creates the volume by extruding the closed curve of line segments in
// 'pt' along the line 'dir'. The Z value for all points in 'pt' is 0.
// NOTE: This does not do auto LOD, bezier curves - meant to be self-contained.
// WARNING: 'pt' is not validated. Beware non-convex or self-intersecting lines
typedef struct Extrud : public BaseAsset {
  // dir is the extruding direction.
  glm::vec3 dir;
  // aspect linearly scales the curve at the end of 'dir'. Minimum of 0.
  float aspect{1.0f};
  // pt defines the 2D closed curve extruded along extent. Minimum of 3.
  std::vector<glm::vec2> pt;
  // toVertices validates the asset, calls eval and writes the result to 'out'.
  virtual int toVertices(VertexIndex& out);
} Extrud;

typedef struct HeightMapPoint {
  float y;
  // flags can contain:
  // VERT_PER_FACE = 1 (bit 0)
  // unused = 2 (bit 1)
  // unused = 4 (bit 2)
  // unused = 8 (bit 3)
  // bits  4-17 (14 bits) signed twos complement x-displacement
  // bits 18-31 (14 bits) signed twos complement z-displacement
  //
  // The x- and z-displacement allow the mesh to be irregular, for creating
  // vertical or even overhanging faces in the mesh. The xz displacement is
  // scaled so that -256 overlaps with 0 of the left neighbor, and +256 overlaps
  // with 0 of the right neighbor. Neighbors would ideally have a gradually
  // lessening amount of the xz displacement so the grid is "mostly" a grid.
  uint32_t flags;
} HeightMapPoint;

// HeightMap creates a surface from a 2D array of height data.
// NOTE: This does not do auto LOD, bezier curves - meant to be self-contained.
// WARNING: data is not validated. Beware non-convex or self-intersecting lines
typedef struct HeightMap : public BaseAsset {
  // pt is a shared_ptr because no BaseAsset can be updated in Library in-place.
  // Any updating must be done by removing and adding a new HeightMap.
  // This shared_ptr allows the new HeightMap to refrence the same vector, and
  // even mutate it (since it is only evaluated when toVertices is called).
  //
  // X and Z values start with (0, y, 0) and range to (width - 1, y, height - 1)
  // where height is pt->size() / width; pt is a linearized 2D array.
  std::shared_ptr<std::vector<HeightMapPoint>> pt;
  // width is used to divide the vector in pt into columns. Minimum of 2.
  size_t width{2};
  // scaleX scales the X axis and is multiplied by aspect to scale the Z axis.
  float scaleX{1.0f};
  // aspect scales the Z axis relative to the X axis.
  float aspect{1.0f};
  // toVertices validates the asset, calls eval and writes the result to 'out'.
  virtual int toVertices(VertexIndex& out);
} HeightMap;

typedef struct AssetLocRot {
  std::shared_ptr<BaseAsset> asset;
  glm::quat rot;
  glm::vec3 loc;
} AssetLocRot;

// CsgOR creates the volume which is the logical OR of each of the child assets
// Each child asset has a euclidean transform (loc+rot) before being ORed in.
// NOTE: CsgOR does not *optimize* the geometry. It just draws all the children
// NOTE: This differs from a Scene, below, in the sense that the geometry of
// this asset, like all assets, is designed to be *immutable* on the GPU, or
// at least *cached*, and changing it will be expensive.
//
// On the other hand, a Scene is designed to be *mutable* on the GPU, but it
// is fast because of the fast instancing of all the immutable assets.
typedef struct CsgOR : public BaseAsset {
  std::vector<AssetLocRot> child;
  // toVertices validates the asset, calls eval and writes the result to 'out'.
  virtual int toVertices(VertexIndex& out);
} CsgOR;

// Library maintains the vertex and index buffers on the GPU (with help from
// your app) for assets - shapes you can render. Library holds vertexBuf and
// indexBuf inside it, in device-local memory.
// TODO: add support for GPUs that prefer host-visible memory (mobile GPUs).
//
// NOTE: This is not a Scene, this is a std::set of assets.
// Your app can load and unload assets on the fly, but keep in mind the design
// goal here is to store vertex and index data on the GPU and not on the CPU.
// Rebuilding the GPU buffers are expensive. Ideally, your app does work on
// the assets up front (load all the assets before starting the scene), or
// at least only occasionally changes the std::set of assets.
//
// Your app can record a cmdBuffer.drawIndexedIndirect() and submit it, one
// for each asset. Set up the indirect buf so that most of those
// drawIndexedIndirect() commands draw nothing, but now your app can start
// doing instanced drawing by setting instanceCount to 1. You would usually
// also update the instance buffer with the location and rotation of each
// instance of the asset.
//
// You can copy BaseAsset::inst.cmd into your indirect buffer and then modify
// it on a frame-by-frame basis. If you add() or del() assets, the command
// buffer must be rebuilt, since the inst.cmd structures have been added or
// deleted. (A clever optimization is to record more drawIndexedIndirect()
// than you need, then dynamically set some number of them so that
// instanceCount is not 0 any more, depending on how many are needed after you
// add() or del())
typedef struct Library {
  Library(UniformGlue& uglue)
      : uglue{uglue}, vertexBuf{uglue.shaders.dev},
        indexBuf{uglue.shaders.dev} {
    uglue.setMoveVertexBuf("using asset::Library instead of UniformGlue!");

    // You must call ctorError to allocate the buffers.
    vertexBuf.info.size = 0;
    vertexBuf.info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    indexBuf.info.size = 0;
    indexBuf.info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  }

  // ctorError allocates fixed-size vertex and index buffers. This puts your
  // app in control of memory usage. If you are running out of space in the
  // buffers and need to dynamically add more buffers, create a new Library
  // or experiment with asset streaming (calling del() on some assets).
  WARN_UNUSED_RESULT int ctorError(size_t vertexSize, size_t instSize,
                                   size_t maxIndices,
                                   size_t bindingIndexOfInstanceBuf);

  // ctorError is a convenience that calls ctorError, above.
  template <typename T, typename U>
  WARN_UNUSED_RESULT int ctorError(size_t maxIndices,
                                   size_t bindingIndexOfInstanceBuf) {
    return ctorError(sizeof(T), sizeof(U), maxIndices,
                     bindingIndexOfInstanceBuf);
  }

  // addVertexAndInstInputs takes the raw VkVertexInputAttributeDescription
  // vector generated by shader reflection and patches it so the instance
  // buffer inputs are read from bindingIndexOfInstanceBuf.
  // This can be called as many times as needed, once for each pipeline.
  WARN_UNUSED_RESULT int addVertexAndInstInputs(
      science::PipeBuilder& pipe,
      std::vector<VkVertexInputAttributeDescription> attrs);

  // addVertexAndInstInputs is a convenience that calls addVertexAndInstInputs
  // above.
  template <typename T>
  WARN_UNUSED_RESULT int addVertexAndInstInputs(science::PipeBuilder& pipe) {
    if (sizeof(T) != vertexSize + instSize) {
      logE("addVertexAndInstInputs<wrong type> %zu want %zu\n",
           sizeof(T), vertexSize + instSize);
      return 1;
    }
    return addVertexAndInstInputs(pipe, T::getAttributes());
  }

  UniformGlue& uglue;
  // vertexBuf is the one vertex buffer for the Library.
  memory::Buffer vertexBuf;

  // indexBuf is a copy of 'order' in device-local memory.
  memory::Buffer indexBuf;

  size_t vertexSize{0};
  size_t instSize{0};
  size_t maxVertices{0};
  size_t maxIndices{0};
  size_t bindingIndexOfInstanceBuf{0};
  size_t getIndicesUsed() const { return order.size(); }
  const std::vector<indicesType>& getIndices() { return order; }

  // reset clears the entire Library, but does not destroy vertex and index
  // buffers on the GPU. Reduces CPU memory usage if you do not call add() or
  // del() after this point.
  int clear() {
    if (vFlight || iFlight || vFence) {
      logE("clear: write is busy\n");
      return 1;
    }
    child.clear();
    order.clear();
    return 0;
  }

  // add puts an asset in the Library. Then call write to generate the
  // vertices and indices for all the assets and upload them to the GPU.
  // It is an error to add() the same asset twice, because that would imply
  // your app would later del() the asset twice (and the second del() would
  // be a logic bug).
  int add(std::shared_ptr<BaseAsset> asset);

  // del removes an asset from the Library. Note that until all in-flight
  // GPU operations are complete, the GPU memory is still used. Call write()
  // each frame to let Library work on freeing up GPU memory.
  int del(std::shared_ptr<BaseAsset> asset);

  // write calls toVertices() on assets in the Library to populate your vertex
  // and index buffers. Call write() even if nothing has been added or removed,
  // because this Library will track when in-flight GPU operations are
  // complete and then do delayed cleanup.
  //
  // Because write() does delayed operations, it can sometimes do *nothing*
  // at all. Always check BaseAsset::state(), for an add() wait until it is
  // READY; for a del() wait until it is INVALID.
  WARN_UNUSED_RESULT int write(command::SubmitInfo& info, VertexWriteFn writeFn,
                               void* userData);

  template <typename U, typename T>
  WARN_UNUSED_RESULT int write(command::SubmitInfo& info,
                               int (*writeFn)(U* userData, const Vertex& vert,
                                              T* dst),
                               U* userData) {
    if (sizeof(T) != vertexSize) {
      logE("Library::write: %zu does not match ctorError(%zu)\n", sizeof(T),
           (size_t)vertexSize);
      return 1;
    }
    return write(info, reinterpret_cast<VertexWriteFn>(writeFn), userData);
  }

  // bind calls bind on the vertex and index buffers in the command buffer.
  // NOTE: Your app must call bind, it allocates the per-framebuf index buffers
  WARN_UNUSED_RESULT int bind(command::CommandBuffer& cmdBuf) {
    VkBuffer vertexBuffers[] = {vertexBuf.vk};
    size_t sizeOfVertexIn = sizeof(vertexBuffers) / sizeof(vertexBuffers[0]);
    VkDeviceSize offsets[] = {0};
    return cmdBuf.bindVertexBuffers(0, sizeOfVertexIn, vertexBuffers,
                                    offsets) ||
           cmdBuf.bindIndexBuffer(indexBuf.vk, 0 /*indexBufOffset*/,
                                  indexBufVkIndexType);
  }

  WARN_UNUSED_RESULT int bindInstBuf(
      command::CommandBuffer& cmdBuf, VkBuffer instBufVk,
      VkDeviceSize instBufOffset) {
    VkBuffer vertexBuffers[] = {instBufVk};
    size_t sizeOfVertexIn = sizeof(vertexBuffers) / sizeof(vertexBuffers[0]);
    VkDeviceSize offsets[] = {instBufOffset};
    if (cmdBuf.bindVertexBuffers(1, sizeOfVertexIn, vertexBuffers,
                                 offsets)) {
      logE("bindVertexBuffers(binding = 1) failed\n");
      return 1;
    }
    return 0;
  }

 protected:
  std::set<std::shared_ptr<BaseAsset>> child;

  // freeV is the list of FreeBlock records for allocating vertices
  std::forward_list<FreeBlock> freeV;
  // freeO is the list of FreeBlock records for allocating indices
  std::forward_list<FreeBlock> freeO;
  size_t totalVertUse{0}, totalOrderUse{0};

  // order is the index buffer (on the CPU).
  std::vector<indicesType> order;

  // vFlight is the write to the vertex buf.
  std::shared_ptr<memory::Flight> vFlight;
  // iFlight is the write to the index buf.
  std::shared_ptr<memory::Flight> iFlight;
  // vFence signals when the writes both are complete.
  std::shared_ptr<command::Fence> vFence;

  // alloc copies 'out' into 'batch.' It assumes batch is relative to
  // firstFreeVert, firstFreeOrder. It implements the heap in vertexBuf and
  // indexBuf. It updates a.inst.cmd.vertexOffset and a.inst.cmd.firstIndex.
  int alloc(size_t& firstV, size_t& firstO, VertexIndex& batch,
            VertexIndex& out, BaseAsset& a);

  // free updates the heap in vertexBuf and indexBuf to free up the blocks used
  // by a.
  int free(BaseAsset& a);
} Library;

}  // namespace asset
