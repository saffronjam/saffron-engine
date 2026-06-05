# Phase 3 — Control: play commands, active-scene routing, schema + se CLI

**Status:** NOT STARTED

**Depends on:** phases 1, 2.

Play mode becomes drivable: five commands, the DTOs and generated artifacts, the sweep that
routes scene reads/writes through `activeScene`, the `se` CLI surface, and the command
reference rows — the full "Keep current" obligation for a new engine capability.

## DTOs (`control_dto.cppm` + `tools/gen-control-dto/gen.ts` catalog)

```cpp
struct PlayStateResult
{
    std::string state;        // "edit" | "playing" | "paused" (playStateName)
    i32 playVersion;
    i32 sceneVersion;         // echoed so a stop is immediately visible as a scene change
    bool hasPrimaryCamera;    // captured at enterPlay; false -> editor warning
};

struct StepParams
{
    std::optional<i32> frames;  // default 1
};
```

Extend `SelectionResult` (`control_dto.cppm:619-624`) with `std::string playState; i32
playVersion;` — the editor's existing fast poll already fetches `get-selection`
(`store.ts:643`), so play-state propagation costs zero new round-trips.

Codegen, all committed in lockstep:

- Add the two DTOs and the five commands to the `gen.ts` catalog (the commands array is the
  single source of the OpenRPC doc *and* the manifest), e.g.
  `{ name: "play", params: "EmptyParams", result: "PlayStateResult", summary: "play — enter or resume play mode" }`
  and siblings for `pause`/`stop`/`step`/`get-play-state`.
- Give each command a `commandFixtures` entry (plus a matching `paramsForFixture` case in
  `check.ts`) — the contract test asserts help↔manifest completeness, so a command without a
  fixture or a `commandSkips` reason fails the gate. The fixture loop runs against **one live
  engine**, so the play fixtures are stateful: order them `play → pause → step → stop` so the
  engine ends back in `edit` and later fixtures see the edit scene. If fixture ordering turns
  out not to be guaranteed, fall back to fixturing only `get-play-state` and skipping the four
  transitions with a stated reason — phase 4's e2e suite carries their behaviour either way.
- Regenerate: `control_dto_serde.generated.cpp`, `schemas/control/openrpc.generated.json` +
  `command-manifest.generated.json`, `editor/src/protocol/` (`bun run gen:protocol`).

## Commands (`control_commands_scene.cpp`)

| Command | Params | Result | Effect |
|---|---|---|---|
| `play` | — | `PlayStateResult` | `Edit→Playing` via `enterPlay`, or `Paused→Playing` via `resumePlay`. Error if already `Playing`. |
| `pause` | — | `PlayStateResult` | `Playing→Paused`. Error otherwise. |
| `stop` | — | `PlayStateResult` | `→Edit` via `stopPlay`. Idempotent in `Edit`. |
| `step` | `{frames?=1}` | `PlayStateResult` | `Paused` only: `stepPlay(ctx, frames)`. Error otherwise. |
| `get-play-state` | — | `PlayStateResult` | Read-only; the CLI/tests poll. |

Handlers are thin `registerCommand<EmptyParams, PlayStateResult>` wrappers over the phase-1
transitions; the result is filled from the context after the transition. Error strings come
from the `Result<void>` returns (the established `std::expected` pattern).

## Active-scene routing sweep

The chokepoint is only as good as the sweep's reach. The rule: **every** occurrence of
`ctx.sceneEdit.scene` / `editor.scene` in command handling becomes `activeScene(...)` —
including the non-obvious shapes:

- Plain handler reads/writes in `control_commands_scene.cpp`: `list-entities`, `inspect`,
  `pick`, `get-selection`, `create-entity`, `add-entity`, `copy-entity`, `destroy-entity`,
  `rename-entity`, `set-parent`, `add/remove/set-component`, `set-component-field`,
  `set-transform`, `set-material`, `set-light`, `select`, `focus`, `deselect`
  (`list-components` reads the registry — unaffected).
- **Local alias initializers** — `Scene& scene = ctx.sceneEdit.scene` inside `destroy-entity`,
  `add-entity`, `copy-entity` — changing other lines while missing these silently splits the
  command across two scenes.
- **Second-stage reads** after a resolve: the ~12 `entityRefDto(ctx.sceneEdit.scene, *entity)`
  sites and `focus`'s `worldTranslation(ctx.sceneEdit.scene, *entity)` — a play-registry
  handle queried against the edit registry is the cross-registry aliasing bug phase 1 warns
  about.
- **Helper bodies, not call sites**: `pickBillboard(SceneEditContext&, …)` reads
  `editor.scene` internally — re-route the body (same rule as the host helpers in phase 2).
- `resolveEntity` (`control_server.cpp:72`) resolves against `activeScene` via phase 1's
  `findEntityByUuid` (replacing its inline uuid loop) — selectors must find runtime entities
  during play.
- Environment commands (`get/set-environment`, `set-atmosphere`) follow `activeScene` too:
  the duplicate carries its own `SceneEnvironment` copy, so sky tuning during play is also
  discard-on-stop.
- **`control_commands_asset.cpp` is in scope** — three commands touch the scene and would
  otherwise corrupt the authored scene during play, breaking the discard guarantee:
  `assign-asset` (writes mesh/material components) and `asset-usages` (reads) route through
  `activeScene`; `delete-asset` joins the stop-first guard below (it clears component usages
  *and* erases the GPU ref the play scene is rendering from — guarding is safer than routing).

Exceptions that stay on the *authored* scene by design:

- `save-scene` (`control_commands_asset.cpp:661`) and project save: always serialize
  `ctx.sceneEdit.scene`. Saving during play is *safe* under the duplicate model (the authored
  scene is untouched), so no engine-side block — the editor greys it out (phase 5) purely to
  avoid "I saved but my play tweaks aren't in the file" confusion.
- `load-scene` (`:678`) / `load-project` / `new-project` / `delete-asset`: error with "stop
  play first" while `playState != Edit`. These swap state out from under an active duplicate;
  explicit is better than an implied stop.
- `set-gizmo` and `gizmo-pointer` no-op (or error) during play, matching the hidden overlay
  (phase 2). **`get-gizmo` stays a working read** — the editor's fast poll calls it every tick
  (`store.ts:643`); if it errored during play the whole `Promise.all` would reject and the
  editor would stop reconciling. The same applies to `get-selection` and `render-stats`:
  nothing in the hot poll may error while playing.

The contract test (`tools/check-control-schema/check.ts`) must stay green; no new id-typed
fields are involved (`playVersion`/`sceneVersion` are plain i32 counters like the existing
stamps).

## se CLI (`tools/se/source/main.cpp`)

`se play`, `se pause`, `se stop`, `se step [frames]`, `se get-play-state` pass through the
generic path; add a one-line formatter for `PlayStateResult`
(`state=playing playVersion=4 camera=ok|missing`) so the scriptable-editor loop reads well:

```sh
se play && se pause && se step 3 && se get-play-state && se stop
```

## Docs (same change)

`docs/content/reference/control-commands.md`: five new rows in the scene-commands table, plus
a sentence in the intro noting `get-selection` now carries `playState`/`playVersion` and that
scene commands address the running scene during play.

## Touched

| What | File | Symbols |
|---|---|---|
| DTOs | `engine/source/saffron/control/control_dto.cppm` | `PlayStateResult`, `StepParams`, `SelectionResult` |
| Commands + sweep | `engine/source/saffron/control/control_commands_scene.cpp` | `registerSceneCommands` (:149), `pickBillboard` |
| Resolver | `engine/source/saffron/control/control_server.cpp` | `resolveEntity` → `findEntityByUuid` |
| Asset sweep + guards | `engine/source/saffron/control/control_commands_asset.cpp` | `assign-asset`, `asset-usages`, `delete-asset`, `load-scene`, `load-project`, `new-project` |
| Codegen | `tools/gen-control-dto/gen.ts` + all generated artifacts | commands array, `commandFixtures` |
| Contract test | `tools/check-control-schema/check.ts` | `paramsForFixture` |
| CLI | `tools/se/source/main.cpp` | the `PlayStateResult` formatter |
| Reference | `docs/content/reference/control-commands.md` | scene table |

## Done when

- [ ] Five commands in the `gen.ts` catalog with fixtures (or stated skips) ending in `edit`.
- [ ] All four generated artifacts regenerated and committed in lockstep.
- [ ] `tools/ci/check.sh` (build → smoke → contract test → frontend build) green.
- [ ] `grep -n 'sceneEdit.scene\|editor.scene' engine/source/saffron/control/` shows only the
      deliberate authored-scene exceptions (`save-*`, guard checks).
- [ ] Shell round-trip against a running host: `se play && se pause && se step 3 && se
      get-play-state && se stop`; `se inspect` of an entity mid-play shows a `set-transform`
      made during play; after `se stop` it shows the authored value; `se play` twice errors
      the second time; `se step` while playing errors.
- [ ] The reference rows in `docs/content/reference/control-commands.md` land in this change.
