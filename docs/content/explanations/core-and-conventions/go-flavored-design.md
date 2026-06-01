+++
title = 'Go-flavored design'
weight = 1
+++

# Go-flavored design

SaffronEngine is C++ written as if it were Go. Small data structs, free functions that
transform them, errors returned as values, and no class hierarchies. When a design
question comes up, the house answer is "how would Go do this?" The rendering code leans
on this constantly, so it helps to know the style before the graphics pages.

## What you use

Structs with public fields and methods, where a method is just a function with a
receiver. Free functions are preferred, since most logic is a pure function over plain
data and that keeps it testable. Concepts act as compile-time interfaces and
`std::function` closures as runtime ones, with `std::variant` for sum types and
`Result<T>` for anything that can fail.

GPU resources are the one exception to the no-operators rule. They live in move-only RAII
wrapper structs that own a Vulkan handle and free it in the destructor. That is resource
management, not the operator overloading the style otherwise bans.

## What is off the table

No inheritance and no `virtual`. There are no base classes. A runtime interface is a
struct of function values — an explicit version of what a Go interface is under the hood.
The clearest example is `Layer` in the [main loop](../../app-lifecycle-and-window/main-loop-and-run/):
a bundle of optional callbacks, not a subclass.

No exceptions, anywhere in engine code. Fallible work returns [`Result<T>`](../error-handling/)
and is checked at the call site. Libraries that can throw are driven through their no-throw
APIs and converted at the boundary (Vulkan-Hpp with exceptions disabled, nlohmann with
`JSON_NOEXCEPTION`, cgltf and tinyobjloader through their C-style returns).

No ternary operator and no operator overloading on our own types. Use `if`/`else` and
named functions like `add(a, b)`. GLM's operators are fine; they belong to GLM.

## Why it holds up in a renderer

Graphics code is where OOP engines usually grow the deepest hierarchies: a `Resource`
base, a `RenderPass` base, a `Material` base. SaffronEngine has none of them. A render
pass is a [`RgPass`](../../frame-and-render-graph/render-graph-overview/) struct with a
closure inside it. A GPU buffer is a move-only struct passed around as a `Ref<T>`. A
component is a plain struct the [registry](../../scene-and-ecs/component-registry/) knows
how to serialize. The data is visible, the control flow is explicit, and there is no
vtable to trace when something goes wrong.

## In the code

| What | File | Symbols |
|---|---|---|
| Full rules | `CONVENTIONS.md` | naming, allowed/prohibited, return-type style |
| Itable pattern | `app.cppm` | `Layer` (struct of closures), `attachLayer` |
| RAII GPU wrappers | `renderer_types.cppm` | `Pipeline`, `Image`, `Buffer`, `GpuMesh`, `GpuTexture` |

## Related

- [Error handling](../error-handling/) — the `Result<T>` half of the style
- [Main loop and run](../../app-lifecycle-and-window/main-loop-and-run/) — the layer itable in action
- [Meta-layer resources](../../vulkan-foundation/) — the move-only GPU wrappers
