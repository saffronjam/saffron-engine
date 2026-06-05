# Phase 4 — e2e: the discard guarantee over the wire

**Status:** NOT STARTED

**Depends on:** phase 3. Independent of phase 5.

The headline correctness property — *stop restores the authored scene exactly* — proven the
way the user experiences it: over the control plane against a headless engine. This is the
language-appropriate home for play-mode behaviour tests (the wire is JSON; `make e2e`).

## `tests/e2e/play.test.ts`

Same harness shape as `hierarchy.test.ts`: `Engine.boot({ SAFFRON_AUTO_EMPTY_PROJECT: "1" })`,
typed `engine.call(...)`, `engine.settle()` between mutation and read-back, validation-clean
log asserted on shutdown.

Cases:

1. **State machine + invariants.** `get-play-state` starts `edit`. `play` → `playing`;
   second `play` rejects; `pause` → `paused`; `step` accepted only now (`step` while
   `playing` rejects, after `stop` rejects); `play` resumes from `paused`; `stop` → `edit`;
   `stop` again succeeds idempotently. `playVersion` strictly increases across transitions.

2. **Camera flag.** In an empty project `play` reports `hasPrimaryCamera: false`; after
   `add-entity {preset: "camera"}` and a fresh `play`, it reports `true`.

3. **The discard guarantee.** Create a cube, set a known transform, save nothing. `play`;
   `set-transform` to a different value; `inspect` shows the *runtime* value (reads route to
   the play scene); spawn an extra entity during play; `stop`; `inspect` shows the authored
   transform bit-equal to pre-play, `list-entities` does not contain the runtime-spawned
   entity, and the entity count matches pre-play. `sceneVersion` changed across the stop (the
   editor-refresh trigger).

4. **Selection across the boundary.** `select` a cube; `play`; `get-selection` still resolves
   the same uuid (the play twin); `stop`; still selected (the authored entity). Then: during a
   second play, select a runtime-spawned entity; `stop`; selection is cleared.

5. **Load guard.** During play, `load-scene` rejects with the stop-first error.

6. **Environment discard.** `set-environment` during play; `stop`; `get-environment` returns
   the authored values.

7. **Asset-path discard.** `assign-asset` onto an entity during play; `stop`; `inspect` shows
   the authored mesh/material refs (the phase-3 asset sweep is what this guards);
   `delete-asset` during play rejects with the stop-first error.

## Touched

| What | File |
|---|---|
| The suite | `tests/e2e/play.test.ts` (new) |

## Verify

- `make e2e` green, including the existing suites (the `activeScene` sweep must not have
  changed edit-mode behaviour — every pre-existing e2e test doubles as a regression check for
  that).
- Log validation-clean across repeated play/stop cycles inside one engine lifetime (the suite
  plays/stops at least three times — the leak-across-sessions check).
