# Phase 4: Control-plane: parentId on list-entities, set-parent command, schema + contract test + se CLI

**Status:** NOT STARTED

<!-- Flip to COMPLETED when the Done-when checklist passes validation-clean (tools/ci/check.sh schema/contract stage green, `bun run gen:protocol` regenerated). Delete this file only after COMPLETED + merged. -->


## Goal

Expose parenting over the wire, schema-first. `list-entities` emits an optional `parentId`
(the entity's `RelationshipComponent.parent` uuid as a decimal string; absent/`"0"` == root)
per entry. A new cycle-guarded `set-parent {entity, parent?}` command reparents (and clears
to root via `parent` absent/`0`), bumps `sceneVersion`, and returns an `entityRef`. The
contract test's u64-precision allowlist widens to cover `parentId` and drives an
add-entity → set-parent → list-entities round-trip. The `se` CLI keeps showing the tree
(`parentId` column) and reaches `set-parent` (default JSON dump is the minimum).

Depends on phases 1 (`RelationshipComponent` + `setParent`/cycle guard live in `Saffron.Scene`)
and 2 (serialize-by-uuid + the `sceneFromJson` resolve pass). This phase adds zero engine
*mechanism* — `setParent` and the cycle guard already exist; it only exposes them on the wire.
Architecture is settled in `plans/scene-hierarchy/phase-0-research-and-architecture.md` (flat
list + `parentId`, engine-authoritative reparent, `parentId` confined to the list-entry type) —
do not re-litigate it here.


## Current state

- `list-entities` (`control_commands_scene.cpp:53`) iterates `forEach<IdComponent, NameComponent>`
  and pushes `{ id: std::to_string(id.id.value), name: name.name }` (`:60`) — flat, no parent.
- `entityRef` (`control_server.cpp:134`) is the shared DTO `{ id, name }` returned by ~12
  commands; `entity-ref.schema.json` is `additionalProperties:false` with `required [id,name]`.
  `entity-list.schema.json:10` has its items `$ref` `entity-ref.schema.json` directly — the list
  item IS the shared ref today, so widening the item means giving it its own type.
- `resolveEntity` (`control_server.cpp:72`) hardcodes `positionalOr(params, "entity", 0)` (`:74`).
  Resolving a `parent` selector needs a generalized resolver or an inline second resolve.
- `destroy-entity` (`control_commands_scene.cpp:85`) is the template for a mutating command that
  `resolveEntity`s, bumps `ctx.sceneEdit.sceneVersion += 1` (`:99`), and returns a result. It
  clears selection only for the destroyed root (`:94`).
- `uuid.schema.json` is `type:string, pattern ^[0-9]+$` — the u64-as-decimal-string rule; any new
  id field MUST `$ref` it, never inline.
- Contract test `assertRawU64` (`check.ts:80`) scans raw result bytes with the allowlist regex
  `/"(?:id|mesh|albedoTexture|skyTexture)"\s*:\s*([^,}\s]+)/g` (`:82`), asserting each is a quoted
  decimal that round-trips as `BigInt`. The `expect()` driver (`check.ts:143`) validates a command's
  result against a schema and runs `assertRawU64`. `add-entity`/`list-entities` are already driven
  (`check.ts:155`/`:159`); `firstResultId` (`check.ts:113`) pulls an id string from raw bytes.
- `se` CLI `printResult` has a `list-entities` formatter (`tools/se/source/main.cpp:137`) printing
  a two-column `id name`; `add-entity`/`copy-entity` print at `:206`. Unhandled commands fall through
  to the UTF-8 `dump(2)` default.
- `editor/scripts/gen-protocol.ts` auto-discovers every `schemas/control/*.schema.json` (sorted) into
  `editor/src/protocol/index.ts`; a new/edited schema flows in on `bun run gen:protocol`. `client.ts`
  has typed `listEntities`/`destroyEntity`/`addEntity` wrappers; `store.ts` calls `listEntities` on
  `sceneVersion` change. The Rust bridge is one generic `control(cmd, params)` passthrough — a new
  command needs zero Rust changes.


## Implementation

### 1. `list-entities`: emit `parentId` per entry

In `list-entities` (`control_commands_scene.cpp:53`), keep the flat array but read each entity's
`RelationshipComponent.parent` and append it. The `forEach<IdComponent, NameComponent>` view does not
include the relationship; fetch it inside the body (every entity has one after phase 1's
`createEntity` default). Emit the parent as a decimal string, omitting it (or emitting `"0"`) for
roots — pick **omit when `parent == 0`** so the field is genuinely optional and root entities carry
no `parentId`:

```cpp
forEach<IdComponent, NameComponent>(ctx.sceneEdit.scene,
    [&](Entity e, IdComponent& id, NameComponent& name)
{
    json entry{ { "id", std::to_string(id.id.value) }, { "name", name.name } };
    const u64 parent = getComponent<RelationshipComponent>(ctx.sceneEdit.scene, e).parent.value;
    if (parent != 0) { entry["parentId"] = std::to_string(parent); }
    entities.push_back(std::move(entry));
});
```

This is read-only — no `sceneVersion` bump. `parentId` is a decimal string (u64-as-string rule),
never a bare integer. Do NOT touch `entityRef` (`control_server.cpp:134`) — leave the ~12 other
commands' DTO untouched (see Risks).

### 2. New `set-parent` command

Add a `registerCommand` near `destroy-entity` (`control_commands_scene.cpp:85`), modeled on it. It
resolves `entity` via `resolveEntity`, resolves `parent` (absent/`0` => root), calls `setParent`
(phase 1), bumps `sceneVersion`, and returns `entityRef`:

```cpp
registerCommand(reg, "set-parent", "set-parent {entity, parent?}",
    [](EngineContext& ctx, const json& params) -> Result<json>
    {
        auto child = resolveEntity(ctx, params);
        if (!child) { return Err(child.error()); }

        Entity newParent{ entt::null };  // absent/0 => detach to root
        const json sel = positionalOr(params, "parent", 1);
        if (!sel.is_null() && !(sel.is_number_unsigned() && sel.get<u64>() == 0)
            && !(sel.is_string() && sel.get<std::string>() == "0"))
        {
            auto resolved = resolveEntitySelector(ctx, sel);   // see below
            if (!resolved) { return Err(resolved.error()); }
            newParent = *resolved;
        }

        auto ok = setParent(ctx.sceneEdit.scene, *child, newParent);  // cycle guard inside
        if (!ok) { return Err(ok.error()); }
        ctx.sceneEdit.sceneVersion += 1;
        return entityRef(ctx.sceneEdit.scene, *child);
    });
```

**Parent resolution.** `resolveEntity` (`control_server.cpp:72`) hardcodes the `"entity"` key, so it
cannot resolve a `parent` selector. Generalize it: extract the selector-to-`Entity` body into a new
free function `resolveEntitySelector(EngineContext&, const json& selector) -> Result<Entity>` (the
uuid-then-name walk, `control_server.cpp:79-131`), and make `resolveEntity` call it with
`positionalOr(params, "entity", 0)`. Then `set-parent` calls `resolveEntitySelector` for the `parent`
arg. Declare `resolveEntitySelector` alongside `resolveEntity` in `command.cppm` so the scene-command
TU sees it. This keeps one selector code path.

**Cycle guard.** Lives in `setParent` (phase 1) — reject making an entity its own ancestor and
self-parent, returning `Err`. The command surfaces that `Err` as `ok:false`. Do NOT duplicate the
walk here.

**Selection.** `set-parent` MUST NOT clear `ctx.sceneEdit.selected` — a reparent keeps the selection
intact (the editor's optimistic relink relies on this). Only `sceneVersion` bumps; `selectionVersion`
does not.

### 3. Schemas: list-entry type + (optional) set-parent params/result

Give the list its own item type so `parentId` lives ONLY there, never on the shared `entity-ref`:

- **New** `schemas/control/entity-list-entry.schema.json` (title `EntityListEntry`): `id`
  (`$ref uuid.schema.json`), `name` (`string`), `parentId` (**optional**, `$ref uuid.schema.json`);
  `required [id, name]`, `additionalProperties:false`. This mirrors `entity-ref` plus the optional
  `parentId`.
- **Edit** `entity-list.schema.json:10`: change `items` from `{ "$ref": "entity-ref.schema.json" }`
  to `{ "$ref": "entity-list-entry.schema.json" }`. Keep `additionalProperties:false`.
- Leave `entity-ref.schema.json` **untouched** (the ~12-command blast radius).
- `set-parent` returns an `entityRef`, validated against the existing `entity-ref.schema.json` — no
  new result schema needed. A typed params schema is optional; the engine accepts named/positional
  args and `set-parent` is a thin selector pair, so skip a params schema unless a later phase wants
  typed request shapes (record that as a deferral, not a gap).

`title` is the generated TS type name — keep `EntityListEntry` PascalCase and stable. `parentId` MUST
`$ref uuid.schema.json` (string-on-wire, never a bare integer). Run `bun run gen:protocol`
(`editor/scripts/gen-protocol.ts` auto-discovers the new sorted file) to regenerate
`editor/src/protocol/index.ts` — the `EntityList` item type becomes `EntityListEntry` with optional
`parentId: string`. Never hand-edit `index.ts`.

### 4. Contract test: widen the u64 allowlist + drive a reparent

In `tools/check-control-schema/check.ts`:

- **Widen `assertRawU64`** (`:82`): add `parentId` to the allowlist regex →
  `/"(?:id|mesh|albedoTexture|skyTexture|parentId)"\s*:\s*([^,}\s]+)/g`. Without this the new field is
  not precision-checked (and if it is ever emitted unquoted, the regression goes unseen). It still
  asserts the quoted-decimal `BigInt` round-trip.
- **Drive set-parent + post-reparent list-entities** in the `expect()` sequence (`:155`+). A second
  entity already exists implicitly only as the cube; add a second `add-entity` (capture its id via
  `firstResultId`), then `set-parent { entity: childId, parent: cubeId }` validated against
  `entity-ref.schema.json`, then `list-entities` validated against `entity-list.schema.json`. After
  the reparent the child entry carries `parentId == cubeId`; `validate()` walks the new
  `entity-list-entry` item type and `assertRawU64` covers the `parentId` token.
- Add a **negative assertion**: `set-parent { entity: cubeId, parent: childId }` (parent onto its own
  descendant) must return `ok:false` — assert the envelope `ok === false` with a non-empty `error`.
  Use the raw `call()` (`check.ts:93`), not `expect()` (which fails on `ok!==true`).
- Add a **detach assertion**: `set-parent { entity: childId, parent: '0' }` returns ok; a following
  `list-entities` has the child entry with no `parentId` (root).

The test re-reads schemas at runtime, so once `entity-list-entry.schema.json` lands and the engine
emits `parentId`, `validate()` and `assertRawU64` cover the new shape with no further wiring.

### 5. `se` CLI: show parent + reach set-parent

In `tools/se/source/main.cpp`:

- **Extend the `list-entities` formatter** (`:137`): add a `parentId` column so the tree is visible
  from a shell. Print `id`, `name`, and `parent` (empty for roots):

```cpp
std::printf("  %-20s  %-24s  %s\n",
    e.value("id", std::string{}).c_str(),
    e.value("name", "").c_str(),
    e.value("parentId", std::string{}).c_str());
```

- **`set-parent`** needs no dedicated branch — it falls through to the UTF-8 `dump(2)` default, which
  prints the returned `entityRef`. That satisfies the keep-current minimum (the command is reachable
  and inspectable from `se`). A one-line `name id parent` formatter is optional polish; skip it unless
  trivial. Argument coercion already maps `se set-parent <child> <parent>` (bare positionals →
  `params["args"]`, consumed by `positionalOr` at index 0/1) and `--parent 0` for detach.

### 6. Editor (light touch — engine-authoritative, ids-as-strings)

The flat-list-with-`parentId` shape lands in `editor/src/protocol/index.ts` via step 3's regen. The
React tree build, expand-state, and drag-reparent are owned by the editor phase of this plan
(`plans/scene-hierarchy/phase-5-*`, the React tree) — this phase only guarantees the wire and a typed
client wrapper:

- Add `client.setParent(id: string, parentId: string | null)` to `editor/src/control/client.ts`,
  routing `control('set-parent', { entity: id, parent: parentId ?? 0 })`. `parentId` stays a **string
  or null** end-to-end — never `Number()` it (u64 precision). Use the `callRaw` pattern; add to
  `CommandResultMap` only if a typed `EntityRef` return is wanted.
- Respect the focus-gated reconcile poll (`store.ts`): `parentId` rides the existing
  `sceneVersion`-keyed `listEntities` lane already in place — no new fan-out call. Do NOT add a poll
  for hierarchy.
- The X11-child viewport overlay constraint binds the React tree (drop indicators must stay in the
  sidebar DOM), but that is the React phase's concern; nothing in this phase paints over the viewport.

Keep the editor changes here minimal (wrapper + regenerated types); the tree UI is its own phase.


## Done when

- [ ] `list-entities` returns an optional `parentId` (decimal string) per entry — present for
      parented entities, absent for roots — and the result validates against the new
      `entity-list-entry.schema.json` via `entity-list.schema.json`.
- [ ] `se set-parent <child> <parent>` then `se list-entities` shows the child under its parent
      (`parentId` column populated); `se set-parent <child> --parent 0` then `se list-entities` shows
      it back at root (no `parentId`).
- [ ] `se inspect <child>` after a reparent reflects the new parent via the serialized
      `RelationshipComponent` (round-trips through `inspect`).
- [ ] `set-parent` bumps `sceneVersion` (a following `get-selection`/`list-entities` poll sees the new
      `sceneVersion`) and leaves `selectionVersion`/`selected` unchanged.
- [ ] `set-parent` onto a descendant returns `ok:false` with a non-empty error (cycle rejected
      server-side, asserted by the contract test).
- [ ] A parented scene round-trips through `writeScene`/`readScene` (or the
      `runSceneSerializationSelfTest` path) with the parent link preserved — verifiable headless via
      `save-project` then `open-project` then `list-entities` showing the same `parentId`.
- [ ] `assertRawU64` covers `parentId` (the allowlist regex includes it; u64 precision preserved as a
      quoted decimal string).
- [ ] `bun run gen:protocol` regenerates `editor/src/protocol/index.ts` with the `EntityListEntry`
      type carrying optional `parentId: string`; `bun run check` typechecks clean.
- [ ] `tools/ci/check.sh` schema/contract stage is green (contract test drives add-entity →
      set-parent → list-entities, validates the new shape, and is validation-clean in the engine log).


## Risks / seams

- **`entity-ref` blast radius.** `entity-ref.schema.json` is returned by ~12 commands and is
  `additionalProperties:false`; adding `parentId` there ripples through every one. Confine `parentId`
  to the new `entity-list-entry` type and leave `entity-ref` untouched.
- **`assertRawU64` will FAIL `parentId`** unless its allowlist regex (`check.ts:82`) is widened in the
  same change — the new u64 string field is not precision-checked otherwise, and an unquoted regression
  slips through.
- **`resolveEntity` hardcodes `params["entity"]`** (`control_server.cpp:74`). Resolving a `parent`
  selector needs the generalized `resolveEntitySelector` (step 2); a copy-paste second resolve would
  drift from the canonical uuid-then-name path.
- **`set-parent` MUST bump `sceneVersion`** or the editor tree never refreshes (the reconcile poll is
  `sceneVersion`-keyed). It must NOT bump `selectionVersion` or clear `selected` — a reparent keeps the
  selection.
- **Optional vs `"0"` for root.** Choosing *omit `parentId` when parent==0* keeps the field genuinely
  optional and the schema clean; the `se` formatter and the editor must both treat absent and `"0"` as
  root. Pick one convention (omit) and keep all three consumers (engine emit, `se` formatter, contract
  test) in agreement.
- **Schema-first sync obligation.** The schema, `dump-schema` reflection, the contract test, and the
  `se` CLI must stay in lockstep (root `AGENTS.md` keep-current). `dump-schema` is unaffected because
  `parentId` rides the list-entry, not a registered component DTO — but the contract test and `se`
  formatter are part of "done", not follow-ups.
