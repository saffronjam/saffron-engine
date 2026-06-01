+++
title = 'VMA allocator'
weight = 3
+++

# VMA allocator

GPU memory in Vulkan is the caller's problem: choose a memory type, allocate it, bind it to each buffer or image. The Vulkan Memory Allocator does that for the engine. Every device-local buffer and image — meshes, textures, offscreen targets, light SSBOs, acceleration structures — goes through one VMA allocator that lives on the renderer for its whole lifetime. Raw management would mean querying memory types, respecting alignment, sub-allocating to dodge the per-device allocation-count limit, and tracking it all by hand; VMA does the heuristics and hands back a clean `(image, alloc)` pair to free.

## The single impl TU

VMA is a single-header C library that needs exactly one translation unit to instantiate its implementation. That TU is the whole of `cmake/vma_impl.cpp`:

```cpp
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
```

Everywhere else the header is included without that define, so it's just declarations. Compiling the implementation once, in its own file, keeps it out of the module units (which couldn't define it twice anyway) and clear of `import std`.

## Creating the allocator

The allocator is created in `newRenderer`, right after the logical device, handed the instance, physical device, device, and API version, and stored as `context.allocator`. The buffer-device-address flag is set only when [ray tracing](../../global-illumination-and-raytracing/raytracing-foundation/) is supported, because acceleration-structure builds feed vertex/index/instance buffer addresses to the GPU:

```cpp
if (renderer.context.rtSupported)
    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
```

The allocator outlives every resource and is the last GPU object torn down in `destroyRenderer`, after every buffer and image is freed.

## Allocating images and buffers

The pattern is the same for color targets, depth targets, cube images, and 3D images: fill the create info and a `VmaAllocationCreateInfo`, then call `vmaCreateImage`, which allocates and binds in one call. Two choices recur for render targets:

- **`VMA_MEMORY_USAGE_AUTO`** lets VMA pick the memory type from how the resource is used instead of hand-picking a property mask. Render targets land in device-local memory; that's all the engine needs to say.
- **`VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT`** gives each target its own allocation rather than a sub-range of a shared block. Targets are recreated on every viewport resize, so a dedicated allocation makes freeing one a clean, isolated operation.

Host-visible buffers (per-frame UBOs/SSBOs, ray-tracing scratch and instance buffers) use `HOST_ACCESS_SEQUENTIAL_WRITE` + `MAPPED` instead, so VMA keeps them persistently mapped and `pMappedData` is written each frame with no `vkMapMemory` round trip.

Freeing is symmetric: `vmaDestroyImage` for images, `vmaDestroyBuffer` for buffers, each taking the handle and its allocation. This is exactly what the move-only [meta-layer wrappers](../meta-layer-resources/) call from their destructors. The `nullptr` allocator guard in each `reset()` makes a moved-from or default wrapper a no-op to destroy.

## In the code

| What | File | Symbols |
|---|---|---|
| Single impl TU | `vma_impl.cpp` | `VMA_IMPLEMENTATION` |
| Allocator creation | `renderer.cppm` | `vmaCreateAllocator`, `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` |
| Image allocation | `renderer_detail.cppm` | `newColorImage`, `newDepthImage`, `newCubeImage`, `newImage3D` |
| Host-visible buffers | `renderer_detail.cppm` | `makeRtBuffer` (`HOST_ACCESS_SEQUENTIAL_WRITE`, `MAPPED`) |
| Freeing via RAII | `renderer_types.cppm` | `Image::reset`, `Buffer::reset` |

> [!NOTE]
> The allocator is *borrowed* by every resource wrapper, never owned by one. Every `Image`/`Buffer`/`GpuMesh`/`GpuTexture` `Ref` must drop before `vmaDestroyAllocator` runs. `destroyRenderer` enforces this by `waitGpuIdle`, resetting all of them, then destroying the allocator last. See [meta-layer resources](../meta-layer-resources/).

## Related

- [Meta-layer resources](../meta-layer-resources/) — the wrappers that hold a `VmaAllocator` and free through it
- [Device & swapchain](../device-and-swapchain/) — where the allocator is created
- [Bindless textures](../../materials-and-pipelines/bindless-textures/) — VMA-allocated images behind the texture array
