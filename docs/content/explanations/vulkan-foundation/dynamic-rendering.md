+++
title = 'Dynamic rendering'
weight = 5
+++

# Dynamic rendering

Classic Vulkan binds attachments through `VkRenderPass` and `VkFramebuffer` objects you build up front and match against your pipelines. The engine has none. It targets Vulkan 1.4 and binds attachments per pass at record time with `beginRendering` / `endRendering`. There is not a single render-pass or framebuffer object anywhere.

The reason is that the pass set changes per frame. Shadows, G-buffer, AO, SSGI, DDGI, and ReSTIR passes all come and go with toggles. A render-pass renderer would have to declare attachment formats and subpass dependencies up front, build framebuffers bound to specific image views, and keep pipelines compatible with the pass they run in — a lot of static plumbing for a moving target. Dynamic rendering makes a pass just a closure plus a list of image views.

## Recording a pass

The whole frame is recorded by `executeRenderGraph`. For each graphics pass it builds the attachment infos, opens a rendering scope, runs the body, and closes the scope:

```cpp
cmd.beginRendering(rendering);
// set viewport/scissor, run pass.execute(cmd)
cmd.endRendering();
```

A compute pass has no attachments, so it skips this and just runs its body after its barriers.

## Attachment infos

`vk::RenderingInfo` is filled fresh each pass from the graph's tracked image views. Each color attachment becomes a `vk::RenderingAttachmentInfo` whose load/store ops and clear value come straight from the pass's declared [`RgAttachment`](../../frame-and-render-graph/passes-and-attachments/). The layout is always `eColorAttachmentOptimal`, because the [render graph](../../frame-and-render-graph/render-graph-overview/) has already emitted the barrier that put the image there — dynamic rendering does *not* transition layouts for you, so the graph owns that.

Several color attachments go into one `setColorAttachments` call, which is how the [thin G-buffer](../../screen-space-and-post/thin-gbuffer/) writes color and normal targets from one MRT pass. A depth attachment, when present, uses `setPDepthAttachment` and `eDepthAttachmentOptimal`.

## MSAA resolve in the attachment

[MSAA](../../anti-aliasing/msaa/) resolve is part of the attachment info, not a separate pass. When a color attachment declares a `resolve` target, the attachment gets a resolve mode and view, so the multisampled image is averaged down into the 1× target at end-of-pass:

```cpp
if (att.resolve)
{
    colorInfo.resolveMode = vk::ResolveModeFlagBits::eAverage;
    colorInfo.resolveImageView = graph.resources[att.resolve->index].view;
    colorInfo.resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal;
}
```

In a render-pass world this would be a resolve attachment in the subpass description. With dynamic rendering it's two fields on the attachment info.

## Dynamic viewport and scissor

After opening the scope, every graphics pass sets a full-area viewport and scissor from the pass extent. The pipelines are built with dynamic viewport/scissor state, so the same pipeline draws at any size without a rebuild — which matters because the offscreen target resizes with the editor panel.

## In the code

| What | File | Symbols |
|---|---|---|
| Begin/end rendering | `render_graph.cppm` | `executeRenderGraph`, `beginRendering`, `endRendering` |
| Attachment infos | `render_graph.cppm` | `vk::RenderingInfo`, `vk::RenderingAttachmentInfo` |
| MSAA resolve | `render_graph.cppm` | `resolveMode`, `resolveImageView` |
| Dynamic viewport/scissor | `render_graph.cppm` | `setViewport`, `setScissor` |
| Feature enable | `renderer.cppm` | `features13.dynamicRendering` |

## Related

- [Render graph overview](../../frame-and-render-graph/render-graph-overview/) — what calls `beginRendering` per pass
- [Passes and attachments](../../frame-and-render-graph/passes-and-attachments/) — the `RgAttachment` the infos are built from
- [Barriers](../synchronization2-and-barriers/) — the layout transitions dynamic rendering does *not* do for you
- [MSAA](../../anti-aliasing/msaa/) — the resolve target folded into the attachment
