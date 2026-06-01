+++
title = 'Frame & render graph'
weight = 4
+++

# Frame & render graph

A pass declares what it does with each resource; the graph derives the barriers and layout
transitions and records the pass body. Nothing writes a pipeline barrier by hand, and app
layers can add their own passes to the cull → scene → UI frame.

## Pages

| Page | Covers | Code |
|---|---|---|
| [Render graph](render-graph-overview/) | why declared usage, the build-execute-per-frame model | `render_graph.cppm` |
| [Passes](passes-and-attachments/) | `RgPass`, MRT `colors`, depth, load/store/clear, the execute closure | `render_graph.cppm` |
| [Barrier derivation](usage-and-barrier-derivation/) | `RgUsage`, `usageInfo`, `applyAccess`, hazard + layout logic | `render_graph.cppm` |
| [Cross-frame layouts](cross-frame-layouts/) | `externalLayout` write-back, imported images, seeded source scope | `render_graph.cppm` |
| [Adding passes](who-can-add-passes/) | engine passes in `beginFrameGraph` vs. layer `onRenderGraph` | `renderer.cppm` |
| [Limits](limits-and-seams/) | single queue, no transient aliasing, no async compute, the seams left | `render_graph.cppm` |
