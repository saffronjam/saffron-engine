# Phase 10 — Material CRUD commands

**Status:** NOT STARTED
**Depends on:** 03, 09

## Goal

The control-plane surface for materials: `material.create / list / get / update / assign`, with DTOs,
schema generation, `se` CLI verbs, and e2e coverage. After this, materials are fully scriptable and
testable from a shell — the AGENTS.md "feature that adds engine state gets a control command" rule.

## Why

The editor (phase 13) drives everything over the control plane, and e2e tests run on the wire. This
phase makes the native material system (phases 01–09) operable without any UI.

## Commands (match the `registerCommand<Params, ResultDto>` pattern)

- **`material.create {name, folder?, from?}` → `{ id }`** — new `.smat`; `from` (entity) snapshots its
  inline `MaterialComponent` into the asset. Registers an `AssetType::Material` catalog entry.
- **`material.list {}` → `{ materials: [{id, name, folder}] }`** — for the browser/picker.
- **`material.get {id}` → `{ material: SmatDto }`** — full resolved `.smat` (factors, texture handles,
  features, blend, doubleSided).
- **`material.update {id, patch, smooth?}` → `{ id }`** — JSON-merge patch of factors/textures/features.
  Mirror `set-material`'s coalesced-write shape (`SetMaterialParams`) but target the **asset**, not an
  entity. Apply atomically per call (full coalesced apply, clean transaction boundary for a future undo).
  Bump a per-material version so resolve caches (phase 09) invalidate and previews/thumbnails regenerate.
- **`material.assign {entity, material, slot?}` → `{ entity }`** — set `MaterialAssetComponent` (or a
  `MaterialSetComponent` slot handle). May fold into the existing `assign-asset` with a new `AssetSlotDto`
  value (`Material`) — prefer extending `assign-asset` for consistency, add `material.assign` as an alias.

## Files to touch

- `engine/source/saffron/control/control_dto.cppm` — `MaterialCreate/List/Get/Update/AssignParams` +
  `...Result` + a `SmatDto`. Extend `AssetSlotDto` with `Material` for `assign-asset`.
- `engine/source/saffron/control/control_commands_asset.cpp` (and/or `control_commands_scene.cpp`) —
  register the handlers; reuse `resolveMaterialAsset`/`saveMaterialAsset`.
- `tools/gen-control-dto/gen.ts` — add the commands to `commands[]`; add `commandFixtures` entries for the
  deterministic ones (`material.create/list/get/update`), and `commandSkips` for `material.import`
  (external files, like `import-model`/`import-texture`). **Regenerate** → `control_dto_serde.generated.cpp`,
  `editor/src/protocol/se-types.ts`, `openrpc.generated.json`, `command-manifest.generated.json`.
- `tools/se/source/main.cpp` — optional `printResult` branches for human-readable `material list`/`get`
  (auto-forwards regardless; only formatting is bespoke).
- `tests/e2e/` — a material suite (create → update → get round-trip → assign → list).

## Steps

1. Define the DTOs + `SmatDto`; extend `AssetSlotDto`.
2. Register the handlers (create/list/get/update/assign) over the phase-03/09 engine fns.
3. Add to `gen.ts` (`commands`, `commandFixtures`/`commandSkips`); regenerate; verify the contract test
   (`tools/check-control-schema`) passes.
4. Add `se` formatting + an e2e suite; assert a validation-clean engine log.

## Gate / done

- `make engine` + `make schema` (control-schema contract) clean; `make e2e` material suite green.
- `se material create/list/get/update/assign` all work against a running engine.
- `make prepare-for-commit` clean. Docs: the material control commands (reference page row).

## Risks

- **Fixture/skip omissions** break `check-control-schema` (the gate). Every new command needs a fixture or
  a skip reason — `material.import` must be a skip (external files).
- **Patch semantics**: define `material.update` merge precisely (absent field = unchanged; explicit null =
  clear). Match the optional-field style of `SetMaterialParams`.
- Keep `material.assign` and `assign-asset(Material)` from diverging — one implementation, one alias.
