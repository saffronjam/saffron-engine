# Saffron code conventions — Go-flavored C++

Saffron is written in a deliberately **Go-like** style: small, data-oriented,
functional, with very limited OOP. When in doubt, ask: *"how would Go do this?"*

## Naming
| Thing | Case | Example |
|-------|------|---------|
| Types (struct, enum, concept) | `PascalCase` | `RenderGraph`, `ImageFormat` |
| Functions & methods | `camelCase` | `createApp`, `submitModel` |
| Variables, params, fields | `camelCase` | `frameIndex`, `swapchainImages` |
| Constants & enum values | `PascalCase` | `MaxFramesInFlight`, `ImageFormat::Rgba16f` |
| Files | `snake_case` | `render_graph.cppm`, `vulkan_context.cppm` |
| Directories | `lowercase` | `engine/source/saffron/rendering/` |
| Namespace | `se` (one package-like namespace) | `se::createApp()` |
| Modules | `Saffron.<Area>` | `Saffron.Rendering` |

(CMake files under `cmake/` keep CMake's own `PascalCase.cmake` convention — they
are not our source.)

## Allowed — Go has these
- `struct`s with public fields and methods (a method is a function with a receiver).
- **Free functions — prefer these.** Pure functions wherever practical (testability).
- **Concepts** as compile-time interfaces (the method set a type must satisfy).
- **Closures / `std::function`** for behavior injection and *runtime* interfaces.
- `std::variant` for sum types; `std::expected<T, Error>` for fallible returns.
- Generics via templates/concepts (Go has generics) — keep them simple and flat.
- **RAII wrapper types for GPU / data-plane resources** (the rendering "meta-layer"):
  a `struct` that owns a Vulkan handle and frees it in its destructor. These are
  move-only (deleted copy, defaulted/explicit move) — the destructor + move
  assignment are *resource management*, NOT the prohibited operator overloading.
  Vulkan is used via **Vulkan-Hpp `vk::`** with `VULKAN_HPP_NO_EXCEPTIONS` (calls
  return results, converted to `std::expected`); never `vk::raii` (it throws).

## Prohibited — Go does not have these
- **Inheritance.** No `: public Base`, no class hierarchies.
- **`virtual` / abstract base classes.** Model a *runtime* interface as a struct of
  function values (an explicit itable — which is exactly what a Go interface is),
  or as a `concept` for compile-time dispatch.
- **Exceptions — entirely.** Never `throw`, `try`, or `catch` in our code. Every
  fallible operation returns `std::expected<T, std::string>` (or `std::expected<void, …>`),
  and the result MUST be checked at the call site **immediately** — never propagate an
  unchecked expected. Third-party libraries that can throw are driven through their
  no-throw APIs/configs and converted to `std::expected` at the boundary.
- **The ternary operator `?:`.** Use `if`/`else`.
- **Operator overloading on our own types.** Use named free functions (`add(a, b)`).
  Third-party libs (e.g. GLM) may use operators internally; that's fine.
- Implicit conversions, RTTI-driven designs, deep encapsulation ceremony.

## Shape of things
- Data and behavior are separated where it aids testing: plain data `struct`s +
  free functions that transform them.
- A "constructor" is a free function `newThing(...) -> Thing` (or `-> expected`).
- Prefer composition over any form of subtyping.
- One namespace, `se`. Modules provide the real boundaries.
