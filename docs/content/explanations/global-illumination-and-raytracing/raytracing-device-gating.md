+++
title = 'RT device gating'
weight = 7
+++

# RT device gating

Ray tracing is optional. The KHR acceleration-structure and ray-query extensions aren't on every
device, and the engine targets Vulkan 1.3 with mostly static dispatch — but the extension entry
points aren't statically exported by the loader. So RT support is *detected* at device bring-up,
the extensions and features are enabled only when present, and the few AS entry points are resolved
manually through `vkGetDeviceProcAddr`. Everything keys off one flag: `rtSupported`.

## Detection at device selection

vk-bootstrap's `enable_extension_if_present` enables an extension when the device has it and does
nothing otherwise, so requesting the RT extensions never fails device creation. Presence isn't
enough, though — the feature bits must also be set. The check is both:

```cpp
const bool hasAsExt = physical.enable_extension_if_present(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
const bool hasRqExt = physical.enable_extension_if_present(VK_KHR_RAY_QUERY_EXTENSION_NAME);
bool rtSupported = hasAsExt && hasRqExt
    && asFeat.accelerationStructure == VK_TRUE && rqFeat.rayQuery == VK_TRUE;
```

The RT feature structs are chained into device creation *only* when `rtSupported`, so a device
without them isn't asked to enable a feature it lacks. When RT is on, the VMA allocator also gets
`BUFFER_DEVICE_ADDRESS` — AS builds feed vertex, index, and instance buffers by device address.

## Resolving the entry points

The acceleration-structure and ray-query functions aren't core, so the loader doesn't export them
statically (the engine otherwise relies on Vulkan-Hpp's static dispatch). When RT is supported, the
five functions the engine calls are resolved through `vkGetDeviceProcAddr` into a small dispatch
table (`getBuildSizes`, `createAccel`, `destroyAccel`, `cmdBuild`, `getAccelAddress`).

If any resolve returns null, `rtSupported` is forced back to false — a device that advertised the
extensions but can't supply the functions is treated as non-RT. The `RtDispatch` table is the only
place these C entry points are held; the rest of the renderer calls through it.

## The gate everywhere downstream

`rtSupported` is a hard precondition for everything in the RT and ReSTIR paths:

- `rtSupported(renderer)` exposes it; `setRtShadows`/`setRestir` are no-ops when it's false.
- `buildTlas` returns immediately if `!rtSupported`.
- `buildBlas` is only called from mesh upload when RT is supported, so `GpuMesh::blas` stays null
  otherwise.

That's why the feature toggles can be wired into the UI and the `se` control plane unconditionally
— on a non-RT device they're inert rather than crashing.

## What stays compiled regardless

The mesh PSO's set 6 (the TLAS binding) and `rayQueryShadow` are only *bound and run* under the
runtime flag, but the shader still declares `RaytracingAccelerationStructure rtScene`
unconditionally. So the compiled SPIR-V carries the `RayQueryKHR` capability even on a device that
will never trace — the binding is present, just never accessed. See
[ray-query shadows](../ray-query-shadows/) for the consequence.

## In the code

| What | File | Symbols |
|---|---|---|
| Extension + feature detection | `renderer.cppm` | device bring-up (`hasAsExt`, `rtSupported`) |
| Entry-point resolution | `renderer.cppm` | the `vkGetDeviceProcAddr` block |
| The dispatch table | `renderer_types.cppm` | `RtDispatch`, `VulkanContext::rtSupported` |
| The public gate | `renderer.cppm` | `rtSupported`, `setRtShadows`, `setRestir` |
| BDA on the allocator | `renderer.cppm` | the `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` branch |

> [!NOTE]
> On the software (llvmpipe) dev GPU the extensions can be present and the RT path activates, but
> it runs at roughly 1 FPS — correctness-validated, awaiting real ray-tracing hardware. The DDGI
> software trace is the everywhere-fast alternative.

## Related

- [Acceleration structures](../raytracing-foundation/) — what the resolved entry points build
- [Ray-query shadows](../ray-query-shadows/) — the runtime-gated consumer
- [Vulkan foundation](../../vulkan-foundation/) — static dispatch + the no-exceptions result convention
