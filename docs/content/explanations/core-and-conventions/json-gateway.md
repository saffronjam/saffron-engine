+++
title = 'JSON gateway'
weight = 7
+++

# JSON gateway

The engine uses nlohmann/json for scene and project files but never calls the library
directly. `Saffron.Json` is a thin gateway that wraps every fallible JSON operation in the
engine's [error-as-value style](../error-handling/), because the way nlohmann is built here
makes the raw API dangerous to touch.

## Why a gateway exists

nlohmann/json compiles with `JSON_NOEXCEPTION`. With exceptions disabled, the library's own
error path is `std::abort()`. A parse error, a `.dump()` on invalid UTF-8, or a typed read
like `get<T>()` or `at()` on the wrong type doesn't throw — it kills the process. So the
gateway's whole job is to convert every operation that *would* abort into a `Result` or a
checked default, so untrusted JSON can never take down the editor.

## Parse and dump

Parsing uses nlohmann's `allow_exceptions = false` overload, which returns a discarded
value instead of aborting, and the gateway turns that into an `Err`:

```cpp
auto parseJson(std::string_view text) -> Result<Json>
{
    Json value = Json::parse(text, nullptr, false);  // allow_exceptions = false
    if (value.is_discarded()) { return Err(std::string{ "invalid JSON" }); }
    return value;
}
```

Serializing is the mirror image. `.dump()` aborts on invalid UTF-8, so `dumpJson` passes
`error_handler_t::replace`, substituting the replacement character instead of dying. A
negative indent is compact; zero or more pretty-prints with that many spaces.

## Typed reads check the type first

The dangerous reads are the typed ones — asking a value for a `u64` it doesn't hold. The
field readers locate the key, verify the stored type, and only then extract. A missing key
and a wrong type each become a descriptive `Err`, never an abort.

```cpp
auto jsonString(const Json& object, std::string_view key) -> Result<std::string>
{
    Json::const_iterator it = findField(object, key);
    if (it == object.end()) { return Err(std::format("missing key '{}'", key)); }
    if (it->is_string())    { return it->get<std::string>(); }
    return Err(std::format("key '{}' is not a string", key));
}
```

`jsonU64` is the most forgiving of the set: it accepts an unsigned number, a non-negative
signed number, *and* a numeric string — because the `se` CLI passes bare numbers across the
socket as strings, and the gateway is the natural place to absorb that.

## Value-or-default variant

Optional fields don't want a `Result` at every call site. For those, each reader has an
`Or` twin that swallows the error and returns a fallback — `jsonU64Or`, `jsonStringOr`,
`jsonF32Or`, `jsonBoolOr`. This is what scene and project loading lean on: a field absent
in older saves reads as its default rather than failing the whole load, which is how a
[project file](../../geometry-and-assets/project-serialization/) stays forward-compatible
as components gain fields.

## In the code

| What | File | Symbols |
|---|---|---|
| Gateway rationale | `json.cppm` | module doc (`JSON_NOEXCEPTION` → abort) |
| Parse / serialize | `json.cppm` | `parseJson`, `dumpJson` |
| Checked typed reads | `json.cppm` | `jsonU64`, `jsonString`, `jsonF64`, `jsonBool` |
| Value-or-default reads | `json.cppm` | `jsonU64Or`, `jsonStringOr`, `jsonF32Or`, `jsonBoolOr` |

> [!WARNING]
> Reach for `Json::get<T>()`, `at()`, or `.dump()` directly and a malformed value aborts
> the process — `JSON_NOEXCEPTION` has no throw to catch. Always go through the gateway's
> checked readers; they are the reason bad input fails gracefully instead of crashing.

## Related

- [Error handling](../error-handling/) — the `Result`/`Err` style the gateway converts into
- [Scene serialization](../../scene-and-ecs/scene-serialization/) — the registry-driven save/load built on these readers
- [Project serialization](../../geometry-and-assets/project-serialization/) — the unified `project.json`
