+++
title = 'Cross-frame layouts'
weight = 4
+++

# Cross-frame layouts

A cross-frame layout is the resting Vulkan image layout an image holds at the end of one frame and
relies on at the start of the next, carried across the frame boundary by a write-back pointer.

Some images live longer than a frame. The offscreen is sampled by ImGui at the end of one frame and
written as a color attachment at the start of the next. The graph is rebuilt from scratch every
frame, which is cheap and keeps per-frame state simple. A rebuilt graph holds no memory of an
image's prior layout, so on the first touch it would emit a transition that is already satisfied.
The write-back pointer preserves that layout so the next frame derives the correct barrier instead.

## Imported, not allocated

The graph never allocates a resource. Every target is an existing renderer-owned Vulkan handle
registered with `importImage` (or `importBuffer`), which returns an `RgResource` index.

```cpp
auto importImage(RenderGraph& graph, vk::Image image, vk::ImageView view,
                 vk::ImageAspectFlags aspect, vk::ImageLayout initialLayout,
                 vk::ImageLayout* externalLayout) -> RgResource;
```

Most imports pass `initialLayout = eUndefined` and `externalLayout = nullptr` — images the graph
fully owns within the frame (depth buffer, MSAA color, G-buffer targets, swapchain image). They
start undefined, get written, and their final layout does not matter. The long-lived imports pass a
real `externalLayout`.

## The externalLayout pointer

A non-null `externalLayout` does two things. On import it seeds the resource's entry layout from
the pointed-to value, ignoring the `initialLayout` argument. At the very end of
`executeRenderGraph`, after every pass has run, it writes the resolved layout back through the
pointer:

```cpp
for (RgResourceState& r : graph.resources)
{
    if (r.externalLayout != nullptr) { *r.externalLayout = r.layout; }
}
```

The pointer is the image's own layout field on the renderer side. The offscreen is imported with
`&offscreen.layout`, so that field both seeds the graph this frame and receives the resolved
layout for next frame.

```mermaid
flowchart LR
    A["offscreen.layout<br/>(renderer)"] -->|seed on import| B[graph resource state]
    B -->|passes run, layout advances| C[resolved layout]
    C -->|write-back on exit| A
    A -.->|next frame| B
```

This keeps the [tonemap](../../screen-space-and-post/tonemap-and-exposure/) and ImGui sampling from
conflicting with the next frame's scene write. The offscreen rests in `ShaderReadOnlyOptimal` after
ImGui samples it; that value is written back; the next frame's import seeds the entry layout from
it, and the first scene `ColorWrite` derives a single correct transition.

## Seeding the source scope

The entry layout alone is not enough. To order the first barrier against an imported image, the
graph also needs the source stage and access — what last touched it. A freshly imported resource
has no prior pass this frame to read that from, so `seedImageState` reconstructs it from the entry
layout:

```cpp
void seedImageState(RgResourceState& r)
{
    if (r.layout == vk::ImageLayout::eShaderReadOnlyOptimal)
    {
        r.lastStage  = vk::PipelineStageFlagBits2::eFragmentShader;
        r.lastAccess = vk::AccessFlagBits2::eShaderSampledRead;
    }
    else
    {
        r.lastStage  = vk::PipelineStageFlagBits2::eTopOfPipe;
        r.lastAccess = vk::AccessFlagBits2::eNone;
    }
}
```

An image that comes in as `ShaderReadOnlyOptimal` was last read by a fragment shader (ImGui, or a
previous sampling pass), so the next write must wait on `eFragmentShader` / `eShaderSampledRead`,
the write-after-read source scope. Any other entry layout has no in-frame predecessor worth waiting
on, so the source defaults to `eTopOfPipe` / `eNone`, ordering against nothing. An incorrect source
scope either over-synchronizes or races the next frame's write against the previous frame's read.

## Which images carry across

| Image | externalLayout | Why |
|---|---|---|
| Offscreen color | yes | sampled by ImGui at end of frame, written at start of next |
| Shadow / spot-shadow map | yes | written by the depth pass, sampled by the scene pass |
| TAA history images | yes | one frame's write is next frame's read |
| DDGI voxel proxy | yes | accumulated across frames |
| Depth, MSAA color, G-buffer | no | produced and consumed within one frame |
| Swapchain image | no | fresh acquire each frame; starts undefined, ends in present layout |

An image gets an `externalLayout` when its contents (or just its layout) must mean something next
frame. A scratch image imports as `eUndefined` and the graph clears it on first
write.

> [!NOTE]
> `seedImageState` only special-cases `ShaderReadOnlyOptimal`. It assumes anything else with no
> in-frame predecessor was last touched at `eTopOfPipe`. That holds because the only images the
> engine carries across frames in another resting layout (the `GENERAL` history images) are always
> *written* first next frame, where the destination scope dominates and the loose source is
> harmless.

## In the code

| What | File | Symbols |
|---|---|---|
| Import + seed entry layout | `render_graph.cppm` | `importImage`, `externalLayout` |
| Reconstruct the source scope | `render_graph.cppm` | `seedImageState` |
| Write the resolved layout back | `render_graph.cppm` | `executeRenderGraph`, `RgResourceState::externalLayout` |
| Persistent layouts in practice | `renderer.cppm` | `beginFrameGraph` (`&offscreen.layout`, `&shadowMap.layout`) |

## Related

- [Render graph](../render-graph-overview/) — why the graph is rebuilt every frame
- [Barrier derivation](../usage-and-barrier-derivation/) — how the seeded source scope feeds the first barrier
- [Passes](../passes-and-attachments/) — the attachments these imported images back
- [Tonemapping and exposure](../../screen-space-and-post/tonemap-and-exposure/) — the pass that leaves the offscreen in ShaderReadOnly
