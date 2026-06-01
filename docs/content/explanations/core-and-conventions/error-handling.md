+++
title = 'Error handling'
weight = 2
+++

# Error handling

Engine code has no exceptions and no error base class. Every operation that can fail
returns its error as a value, which keeps the failure path visible in ordinary control
flow instead of letting it drift upward invisibly. The whole scheme is two declarations
in `Saffron.Core`:

```cpp
template <typename T>
using Result = std::expected<T, std::string>;

inline auto Err(std::string message) -> std::unexpected<std::string>
{
    return std::unexpected<std::string>(std::move(message));
}
```

`Result<T>` is exactly `std::expected<T, std::string>`. Success is the value itself;
failure is `Err("message")`. There is no `Ok(...)` wrapper — you return the value
directly, or `{}` for `Result<void>`.

## Check at the call site

A `Result` is checked immediately and never propagated unchecked. The pattern reads the
same everywhere:

```cpp
auto windowResult = newWindow(config.window);
if (!windowResult)
{
    logError(std::format("failed to create window: {}", windowResult.error()));
    return 1;
}
```

You see this shape in `run`, where the window, renderer, and UI are each created and
checked in turn, and each failure cleans up what came before it and bails. Because there
are no exceptions, that cleanup is ordinary code, not a stack-unwinding side effect.

## Why a string, not an enum

The error type is a plain `std::string`. That trades programmatic matching for a readable
message at the point of failure, which is the right trade for an engine where most errors
are "this Vulkan call returned this result" or "this file failed to parse". Callers that
react differently to different failures branch on specific calls rather than inspecting an
error code.

## At the third-party boundary

The no-exceptions rule extends to libraries that would otherwise throw. They are driven
through their no-throw surfaces and converted to `Result` at the boundary: Vulkan-Hpp
compiles with `VULKAN_HPP_NO_EXCEPTIONS` so calls return a result code; nlohmann/json
compiles with `JSON_NOEXCEPTION`; cgltf and tinyobjloader expose C-style return values
that map cleanly onto `Result`.

> [!WARNING]
> `JSON_NOEXCEPTION` turns what would be a thrown exception into a call to `std::abort`. A
> `Result`-returning JSON path still has to avoid the operations that would throw — it
> parses defensively and validates before indexing. The no-throw build does not make bad
> access safe, it makes it fatal.

## In the code

| What | File | Symbols |
|---|---|---|
| The types | `core.cppm` | `Result`, `Err` |
| The rule | `CONVENTIONS.md` | "Return types" and "Prohibited" |
| A real chain of checks | `app.cppm` | `run` — window → renderer → UI |
| The JSON gateway | `json.cppm` | `JSON_NOEXCEPTION` parse/access helpers |

## Related

- [Go-flavored design](../go-flavored-design/) — the style this is part of
- [Vulkan foundation](../../vulkan-foundation/) — where Vulkan results become `Result`
