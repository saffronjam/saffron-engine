+++
title = 'Layers'
weight = 2
+++

# Layers

A layer is a unit of program behavior the app runs at each phase of the frame. It is a plain
struct of optional `std::function` callbacks, not a class to subclass, and it has no virtual
methods. The app keeps a list of layers and invokes whichever callbacks each one has set.

This is the Go-interface-as-itable pattern: the "interface" is the set of function values, and
an implementation is the set of callbacks assigned into them. The shape mirrors a conventional
`App`/`Layer` lifecycle while dropping the inheritance that usually comes with it.

```cpp
struct Layer
{
    std::string name;
    std::function<void()> onAttach;
    std::function<void(TimeSpan)> onUpdate;
    std::function<void()> onRender;                   // submit GPU work; runs inside the frame
    std::function<void()> onUi;
    std::function<void(RenderGraph&)> onRenderGraph;   // add passes to the frame graph
    std::function<void()> onDetach;
};
```

## How dispatch works

A client builds a `Layer`, fills in the callbacks it needs, and pushes it with `attachLayer`.
`App` owns the layers in a flat `std::vector<Layer>` in attach order. At each phase of the
frame, `run` walks the vector and calls only the layers that set that callback:

```cpp
for (Layer& layer : app.layers)
{
    if (layer.onUpdate) { layer.onUpdate(delta); }
}
```

The null check is the whole dispatch mechanism. An unset callback is a null `std::function`
the loop skips, so a layer that only draws UI sets `onUi` and leaves the rest empty. There is
no base class forcing stubs for methods a layer does not use.

## The callback set

Each callback maps to a fixed point in the loop (see [the main loop](../main-loop-and-run/)):

| Callback | When it runs | Typical use |
|---|---|---|
| `onAttach` | once, after `onCreate`, before the loop | allocate resources, subscribe to window signals |
| `onUpdate(dt)` | every iteration, first | game logic, camera, animation; `dt` is a wall-clock `TimeSpan` |
| `onRender` | inside the frame, after `beginFrame` | record GPU work through the [submit seam](../the-submit-and-rendergraph-seams/) |
| `onUi` | inside the frame, between `uiBeginFrame` and `uiEndFrame` | ImGui panels |
| `onRenderGraph(graph)` | inside the frame, after the engine's passes are added | add passes to the live frame graph |
| `onDetach` | once, during teardown, before `onExit` | drop resource `Ref`s |

The two rendering callbacks are separate on purpose. `onRender` records commands into the
current frame; `onRenderGraph` is handed the live graph so the layer can add whole passes.
That split is the subject of [the submit and render-graph seams](../the-submit-and-rendergraph-seams/).

## Why closures, not a virtual base

A struct of `std::function` offers two advantages over a virtual base. Every callback is
independently optional, with no empty-override boilerplate. And a layer can capture its state
in the closures rather than in member fields of a derived type, so the same struct works
whether the behavior lives in a lambda, a free function, or a bound method. The cost is one
indirect call per set callback per phase, negligible next to a frame's GPU work.

The editor is built this way. It is a layer whose closures hold the editor state and wire the
hierarchy, inspector, and viewport into `onUi` and the camera into `onUpdate`.

## In the code

| What | File | Symbols |
|---|---|---|
| Layer struct | `app.cppm` | `Layer` |
| Attaching | `app.cppm` | `attachLayer`, `App::layers` |
| Dispatch | `app.cppm` | `run` — the per-phase `for (Layer&) if (callback)` walks |

> [!TIP]
> Layers run in attach order at every phase, and there is no priority or removal API. If one
> layer's `onUpdate` must see the result of another's, attach it second. The order you call
> `attachLayer` in `onCreate` is the order everything runs for the life of the program.

## Related

- [Main loop](../main-loop-and-run/) — where the callbacks are invoked
- [Render seams](../the-submit-and-rendergraph-seams/) — what `onRender` vs `onRenderGraph` do
- [Window and events](../window-and-events/) — the signals a layer subscribes to in `onAttach`
