+++
title = 'Logging'
weight = 5
+++

# Logging

Logging is three free functions in `Saffron.Core` that write a tagged line to stdout. No
logger object, no severity filtering, no sinks. That is enough for an engine that does most
of its real diagnosis through Vulkan validation and the
[`se` control plane](../../tooling-and-control/control-plane-architecture/).

## Three functions

```cpp
void logInfo(std::string_view m)  { std::println("[saffron] {}", m); }
void logWarn(std::string_view m)  { std::println("[saffron] warn: {}", m); }
void logError(std::string_view m) { std::println("[saffron] error: {}", m); }
```

Each takes a `std::string_view`, prefixes `[saffron]` (with `warn:` or `error:` for the
non-info levels), and prints with `std::println`. There is no level you can mute and no
timestamp — the prefix is the whole protocol, which makes engine output trivially
`grep`-able.

## Build the message at the call site

The functions take a finished string, so any formatting happens at the call site with
`std::format`. That keeps the logging surface tiny and pushes the interesting part to where
the context is. It pairs naturally with a `Result` check:

```cpp
if (!windowResult)
{
    logError(std::format("failed to create window: {}", windowResult.error()));
    return 1;
}
```

That is the same shape the [error-handling page](../error-handling/) shows: a failed
`Result` carries a string message, and `logError` surfaces it before the function bails.

## Why it stays this small

A heavier system — categories, levels, async sinks — would be infrastructure the engine
doesn't currently need. Validation layers catch the Vulkan mistakes, the control plane
makes the running editor inspectable from the CLI, and prefixed stdout covers the rest. The
three functions are a seam: if structured logging is ever wanted, the call sites already
funnel through them.

## In the code

| What | File | Symbols |
|---|---|---|
| The functions | `core.cppm` | `logInfo`, `logWarn`, `logError` |
| A real error path | `app.cppm` | `run` — `logError` on a failed `Result` |

## Related

- [Error handling](../error-handling/) — where `logError` reports a failed `Result`
- [Control plane architecture](../../tooling-and-control/control-plane-architecture/) — the richer way to inspect a running editor
