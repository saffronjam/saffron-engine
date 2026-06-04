+++
title = 'Serialization'
weight = 5
+++

# Serialization

Scene serialization converts a live entt scene into a JSON document and back, preserving every
entity, its components, and its stable identity. The save and load paths are registry-driven: they
hold no per-component code, instead walking entt storage and asking the
[component registry](../component-registry/) what each thing is.

This keeps the format open to extension. Adding a component to the registry makes it serializable
without touching the save/load path, and the round-trip is symmetric — a document written by one
build reads back into an equivalent scene.

## The document shape

`sceneToJson` produces a `{ version, entities: [...] }` document. Each entity is its Uuid plus a
map of component name to component JSON:

```json
{
  "version": 1,
  "entities": [
    {
      "id": 7421550...,
      "components": {
        "Name": { "name": "Cube" },
        "Transform": { "translation": {"x":1,"y":2,"z":3}, "scale": {...}, "rotation": {...} }
      }
    }
  ]
}
```

`sceneToJson` returns the document without file IO, so it can be embedded inside the larger
`project.json` (see [project serialization](../../geometry-and-assets/project-serialization/)).
`writeScene` and `readScene` add the file layer on top.

## Serialize: walk storage, look up by id

`serializeEntity` iterates the entity's storage sets and, for each one whose id the registry knows,
calls that row's `serialize` closure:

```cpp
for (auto&& [id, set] : scene.registry.storage())
{
    if (!set.contains(entity.handle)) continue;
    const ComponentTraits* traits = findById(reg, id);
    if (traits == nullptr) continue;   // unregistered/internal storage — skipped
    components[traits->name] = traits->serialize(scene, entity);
}
```

A storage with no registered row is skipped silently. That is how `IdComponent` stays out of the
`components` map — it is written as the top-level `id` instead. `sceneToJson` iterates
`forEach<IdComponent>`, so every entity with an identity gets an entry.

## Deserialize: look up by name, add then fill

`deserializeEntity` reads each JSON key, finds the row by name, and runs its `deserialize` closure.
That closure adds the component with defaults if missing, then fills it from JSON. An unknown key
logs a warning and is skipped rather than failing the load, so a file from a build with an extra
component still opens. A parse failure inside a known component propagates as a
[`Result` error](../../core-and-conventions/error-handling/) with the component name prefixed.

```cpp
const ComponentTraits* traits = findByName(reg, it.key());
if (traits == nullptr)
{
    logWarn(std::format("unknown component '{}', skipping", it.key()));
    continue;
}
auto result = traits->deserialize(scene, entity, it.value());
if (!result) return Err(std::format("{}: {}", it.key(), result.error()));
```

## UUID stability

entt entity handles are recycled and not stable across runs, so they cannot serve as the on-disk
identity. Every serialized entity instead carries a [Uuid](../built-in-components/) in its
`IdComponent`, and that is what gets written. The load path does not call `createEntity`, which
would mint fresh Uuids; it preserves the stored ids:

```cpp
scene.registry.clear();
for (const nlohmann::json& entry : doc["entities"])
{
    const u64 uuid = jsonU64Or(entry, "id", 0);
    entt::entity handle = scene.registry.create();
    scene.registry.emplace<IdComponent>(handle, Uuid{ uuid });
    // ... deserialize components ...
}
relinkHierarchy(scene);
```

> [!NOTE]
> Cross-entity references resolve only after the loop. The [scene hierarchy](../scene-hierarchy/)
> stores each entity's parent as a Uuid, and a child's entry may precede its parent in the array,
> so `relinkHierarchy` maps every stored parent Uuid to a live handle once all entities exist. A
> pre-hierarchy document simply has no Relationship keys, and every entity loads as a root.

## Versioning

The document carries `version` (`SceneVersion`, currently `3`: 1 = entities only, 2 = adds the
top-level environment block, 3 = adds the per-entity Relationship component). `sceneFromJson`
rejects anything newer up front rather than guessing at an unknown layout, and migrates older
documents by defaulting what they lack. Bumping the version announces a breaking layout change. A
headless `runSceneSerializationSelfTest` registers Name, Transform, and Relationship, writes a
scene and reads it back, and asserts the hierarchy cases too: parent uuids survive the round trip,
a child entry before its parent still resolves, a v2 document migrates every entity to root, and a
dangling parent downgrades to root with a warning — all with no GPU.

> [!WARNING]
> nlohmann/json is compiled `JSON_NOEXCEPTION`, which turns a would-be throw into `std::abort`.
> The load path therefore validates before it indexes — `is_object`, `contains`, `is_array` —
> and reads fields through `jsonU64Or`-style helpers that default rather than throw. A malformed
> file returns an `Err`, it does not crash.

## In the code

| What | File | Symbols |
|---|---|---|
| Per-entity to/from JSON | `scene.cppm` | `serializeEntity`, `deserializeEntity` |
| Whole scene to/from JSON | `scene.cppm` | `sceneToJson`, `sceneFromJson` |
| File layer | `scene.cppm` | `writeScene`, `readScene` |
| Version + round-trip test | `scene.cppm` | `SceneVersion`, `runSceneSerializationSelfTest` |
| Stable identity | `core.cppm` | `Uuid`, `newUuid` |

## Related
- [Component registry](../component-registry/) — what `serialize`/`deserialize` dispatch through
- [Project serialization](../../geometry-and-assets/project-serialization/) — where this scene doc is embedded
- [JSON gateway](../../core-and-conventions/json-gateway/) — the no-throw parse/access helpers
- [Error handling](../../core-and-conventions/error-handling/) — the `Result` the load path returns
