# Phase 2 — route the in-scope scene edits through history

**Status:** NOT STARTED

**Depends on:** phase 01 (`lib/undo.ts` — the `UndoableEdit` record + `TabHistory`
push/undo/redo state machine — and the store slice's `pushEdit` / `beginEdit` /
`historyByTab` / `historyReplaying`). Phase 01 ships and unit-tests the engine of the
feature in isolation; nothing routes a real edit through it until this phase.

## Goal

Every in-scope scene-tab mutation records an `UndoableEdit` on the scene tab's history
(`historyByTab["scene"]`) at the moment the edit fires — the prior value captured before
the optimistic overlay touches it, the inverse expressed with the same `client.ts`
commands a normal edit uses. Ctrl+Z is **not** wired here (phase 03), so validation is by
inspecting `historyByTab["scene"].past`: a gizmo drag and an Inspector field scrub each add
exactly **one** entry per gesture (collapsed across the coalesced stream by the existing
drag brackets), and each discrete edit pushes exactly one entry with a sensible label and
the right selection id. Recording is purely additive — coalescing, optimism, and the
reconcile poll behave exactly as before.

The engine stays unaware of undo. This phase adds no command: every inverse is an existing
`client.ts` call fed the value the editor already held.

## What exists to build on

- **`control/client.ts`** is the full mutation surface every inverse replays:
  `setTransform(id, partial, smooth?)` and `setMaterial(id, partial, smooth?, slot?)`
  (server-merge, `smooth` for mid-drag animation); `assignAsset(entity, slot, asset)`;
  `setComponentField(id, component, field, value)` and `setComponent(id, component, body)`
  (the latter rewrites the whole component, no merge); `addComponent(id, component)` /
  `removeComponent(id, component)`; `renameEntity(id, name)`; `setParent(id, parentId)`
  (`null` → root); `setScriptOverride(id, slot, name, value)` (`null` clears);
  `setEnvironment` / `setAtmosphere`; the render setters `setAa` / `setClustered` / `setIbl`
  / `setSsao` / `setContactShadows` / `setSsgi` / `setShadows` / `setGi` / `setDepthPrepass`
  / `setRtShadows` / `setRestir` / `setExposure`. `addEntity` / `createEntity` / `copyEntity`
  / `instantiateModel` each return the new id.
- **`panels/InspectorPanel.tsx`** is the densest edit site. `componentsObj[component]` is the
  component's current DTO *before* `onFieldChange` overlays it. The scrub brackets are
  `onDragStart` (`setDragActive(true)`) and `onFieldDragEnd` (`setDragActive(false)` + the
  final exact re-push); a discrete typed edit runs through `onFieldChange` → `coalescerFor`
  with no bracket. `sendWrite(component, field, payload)` routes by component to the right
  command (`assignAsset` for the uuid asset slots, `setTransform` / `setMaterial` for those
  two merge components, `setComponent` for everything else) — reuse it for the inverse.
  The MaterialSet slot path is parallel: `onSlotFieldChange` / `slotCoalescerFor` /
  `onSlotFieldDragEnd`, sending via `setMaterial(id, partial, smooth, slotIndex)`.
  Add/remove component is `onAdd` / `onRemove`.
- **`components/ScriptSlots.tsx`** owns the script-override edits: `onOverride(slotIndex,
  name, value)` reads `slot.overrides[name]` (the prior value; absent ⇒ the field default),
  overlays optimistically, and pushes through `overrideCoalescer` → `setScriptOverride`. Its
  `drag` bracket (`onDragStart` / `onDragEnd` flipping `dragActive`) wraps the scrubbable
  widgets.
- **`panels/HierarchyPanel.tsx`** + the store's `setParent`. `onRenameCommit(id, next)` reads
  the trimmed new name and calls `renameEntity`; the prior name is `store.entities`
  (`entity.name`). The store's `setParent` action already snapshots `const previous =
  …entities.find(e => e.id === id)?.parentId` for rollback — exactly the prior value the
  inverse needs — and bumps sceneVersion / succeeds only on the non-rejected path.
- **`panels/ViewportPanel.tsx`** brackets the gizmo gesture as `begin` (in `onPointerDown`)
  and `end` (in `finishPress`, the `wasDragging` branch, right after `setDragActive(false)`),
  with `setDragActive` around the drag. The viewport streams **NDC coords**, never
  transforms, so the new transform is not available from the gesture itself.
  `store.gizmo.op` (`"translate" | "rotate" | "scale"`) labels the gesture. A plain click
  runs `runPick`.
- **`panels/RenderPanel.tsx`** holds the render toggles in its `TOGGLES` array — each entry's
  `field` is the `renderStats` key and `set` is the matching `client.setX` thunk (the `DDGI`
  row's `set` maps the boolean to `setGi("ddgi"|"off")`) — driven through `onToggle(field,
  set, next)`. Exposure is a `NumberDrag` with `setDragActive` brackets calling
  `client.setExposure(ev)`. The prior of each is `store.renderStats[field]` before the write.
- **`panels/EnvironmentPanel.tsx`** reads `store.environment` (the `env` local) for the
  current value before a write, with `patch(field, value)` / `patchAtmos(field, value)` and
  `onDragStart` / `onDragEnd` brackets on the scrubbable rows.
- **`app/CreateMenu.tsx`** creates entities: `create(preset)` → `client.addEntity` and the
  Named-Empty flow → `client.createEntity`, both selecting the returned `ref.id`.
  `HierarchyPanel.onCopy` → `client.copyEntity`; the `ViewportPanel` drop handler →
  `store.instantiateModel`.
- **`control/coalesce.ts`** `makeCoalescer`: the one-in-flight write coalescer. The history
  hooks live at the *same* gesture boundary the coalescer + `dragActive` already use, so
  "one in-flight coalesced stream" and "one undo entry" share a boundary by construction.
- **The store** from phase 01: `beginEdit({ prior, selectionId })` returning a transaction
  whose `commit(build)` yields exactly one `pushEdit`; `pushEdit(edit)` for discrete edits;
  the `historyReplaying` re-entrancy guard (read via `useEditorStore.getState()`).

## Work

Every recording path opens with the same guard so a replayed inverse never records itself:

```ts
if (useEditorStore.getState().historyReplaying) return;
```

Place it at the *recording* call (the `pushEdit` / `commit` site), never around the control
call — the control call must still run during a replay; only the recording is suppressed.

### 1. Inspector field scrub (gesture) and discrete typed edit

The scrub and the typed-Enter edit share `onFieldChange` but differ in their transaction
boundary, so distinguish them with a ref set at gesture start — by the gesture, not by
comparing values (a typed value can equal a scrubbed one).

1. Add a `gestureRef` (`useRef<{ component: string; field: string; prior: unknown;
   selectionId: string } | null>(null)`). In `onDragStart`, set it from the live
   *pre-overlay* state: `prior = componentsObj[component]` (the DTO before
   `applyOptimisticComponent` mutates it), structurally copied so a later optimistic write
   cannot mutate the stored prior, and `selectionId = selectedId`. Thread the `(component,
   field)` through from the `renderField` `onDragStart` callback so the ref names the right
   field.
2. In `onFieldDragEnd(component, field)`, after the existing `setDragActive(false)` + final
   re-push, read the committed value from the live `componentsBySelected`
   (`useEditorStore.getState().componentsBySelected?.components?.[component]` — the same
   source the re-push reads) and build one `UndoableEdit`:
   - `undo`: route the **prior** value through the same `sendWrite` path with no `smooth`.
   - `redo`: route the committed value the same way.
   - `label`: `humanizeFieldName(field)` (the helper the Inspector renders with).
   - `selectionId`: the captured id.
   Push it through the gesture's `beginEdit` / `commit` so the stream yields exactly one
   `pushEdit`. Clear `gestureRef`.
3. For a discrete typed edit (no drag bracket fired), record in `onFieldChange`'s commit
   path: capture `prior = componentsObj[component]` *before* `applyOptimisticComponent`,
   then push one `UndoableEdit` (same `undo` / `redo` / `label` shape) immediately. Gate on
   `gestureRef.current === null` so a scrub does not also record here.

### 2. MaterialSet slots, add/remove component, script overrides

1. **MaterialSet slot scrub** mirrors §1 on the slot path. Capture the prior slot DTO at the
   slot's `onDragStart`; in `onSlotFieldDragEnd(slotIndex, field)` build the edit with `undo:
   () => client.setMaterial(id, prior, false, slotIndex)` and `redo` sending the committed
   slot value via `setMaterial(id, committed, false, slotIndex)`. One `pushEdit` per gesture;
   label `humanizeFieldName(field)`.
2. **Add component** (`onAdd`): after `addComponent` resolves, push `{ undo: () =>
   client.removeComponent(id, component), redo: () => client.addComponent(id, component),
   label: `Add ${component}`, selectionId }`.
3. **Remove component** (`onRemove`): capture the **full component body first** from
   `componentsObj[component]` before calling `removeComponent`, then push `{ undo: async ()
   => { await client.addComponent(id, component); await client.setComponent(id, component,
   priorBody); }, redo: () => client.removeComponent(id, component), label: `Remove
   ${component}`, selectionId }`. The inverse re-adds the component then restores its exact
   prior body (add alone gives engine defaults, not the user's removed values).
4. **Script overrides** (`ScriptSlots.onOverride`): capture the prior override value
   (`slot.overrides[name]`, possibly `undefined`) before the optimistic write. A scrubbable
   field gesture-groups via the existing `drag` bracket (like §1); a discrete toggle/text
   commit pushes immediately. The edit: `{ undo: () => client.setScriptOverride(entityId,
   slotIndex, name, prior ?? null), redo: () => client.setScriptOverride(entityId, slotIndex,
   name, value), label: humanizeFieldName(name), selectionId: entityId }` — `prior ?? null`
   because a cleared override sends `null`.

### 3. Viewport gizmo gesture → one entry per gesture

The viewport never sees a transform, only NDC; the new transform must be read once *after*
the gesture ends, off the hot path.

1. In `onPointerDown` (the `begin` branch), if `useEditorStore.getState().selectedId` is set,
   capture its prior `Transform` from `componentsBySelected.components.Transform` (a
   structural copy) and open a `beginEdit` keyed on that id. A press with no selection
   captures nothing.
2. In `finishPress`'s `wasDragging` branch, after `setDragActive(false)`, read the new
   transform with **one** `await client.inspect(selectedId)` and pull its `Transform`
   component. Commit one edit: `{ undo: () => client.setTransform(id, priorT), redo: () =>
   client.setTransform(id, newT), label: store.gizmo.op, selectionId: id }`. No `smooth` on
   either side — a replay snaps.
3. The non-drag branch (a plain click → `runPick`) records nothing. A drag that began with
   no selection records nothing (nothing was captured).

### 4. Discrete scene edits → one entry each

1. **Rename** (`HierarchyPanel.onRenameCommit`): capture the prior name from
   `store.entities.find(e => e.id === id)?.name` before `applyOptimisticEntityName`; push
   `{ undo: () => client.renameEntity(id, priorName), redo: () => client.renameEntity(id,
   trimmed), label: "Rename", selectionId: id }`.
2. **Reparent**: record **inside the store's `setParent` action**, not the panel, on the
   success path only — the action already holds `previous` and already rolls back the
   rejected path, so a drag-drop and any future reparent affordance both record once and a
   rejected reparent records nothing. After `await client.setParent(id, parentId)` resolves
   (before the `finally`), push `{ undo: () => client.setParent(id, previous ?? null), redo:
   () => client.setParent(id, parentId), label: "Reparent", selectionId: id }`.
3. **Environment / atmosphere** (`EnvironmentPanel`): capture the prior field value from
   `env[field]` / `atmos[field]` before `patch` / `patchAtmos`. A discrete edit (Select,
   Switch, AssetPicker) pushes one entry; a scrub gesture-groups via the existing
   `onDragStart` / `onDragEnd` brackets. Inverse `client.setEnvironment({ [field]: prior })`
   / `client.setAtmosphere({ [field]: prior })`; redo sends the new value the same way. Label
   `humanizeFieldName(field)`; no `selectionId` (environment is scene-global).
4. **Render toggles + exposure + AA + GI** (`RenderPanel`): the prior is
   `store.renderStats[field]` before the write. A toggle pushes `{ undo: () => set(prior),
   redo: () => set(next) }` reusing the same `set` thunk from the `TOGGLES` row (record the
   boolean and let the `DDGI` thunk map it). Exposure scrubbing gesture-groups via its
   `NumberDrag` brackets and inverts with `client.setExposure(prior)`. AA inverts with
   `client.setAa(priorMode)`. Label from the toggle's `label` / "Exposure" /
   "Anti-aliasing"; no `selectionId`.

### 5. Entity creation — undo-only this phase

Creation mints a new id, so re-creation cannot reuse it; v1 records create as undo-only and
marks it non-redoable (phase 06 sets the drop-on-redo policy and whether the entry truncates
the redo stack). For each creation site — `CreateMenu.create` / the Named-Empty flow (the
`addEntity` / `createEntity` paths), `HierarchyPanel.onCopy` (`copyEntity`), and the
`ViewportPanel` drop handler (`instantiateModel`) — after the new id resolves, push `{ undo:
() => client.destroyEntity(newId), redoable: false, label: "Create entity" (or "Duplicate
entity" / "Add to scene"), selectionId: newId }`. `redoable: false` is the `UndoableEdit`
flag phase 01 defined; the redo thunk is omitted.

## Validation (done criteria)

Run inside the `saffron-build` toolbox: `cd editor && bun run check` (gen:protocol + tsc)
and `bun run lint` (oxlint) both clean. `make prepare-for-commit` clean (this phase touches
no C++ — the engine stays unaware).

Behavioural checks (dev-mode render counters on; inspect `historyByTab["scene"].past` from
the console or a temporary dev readout):

- A gizmo drag adds **one** entry, never N — labelled by the gizmo op, carrying the dragged
  entity's id, `undo` / `redo` both `setTransform` with the prior / final transform.
- A slider/scrub in the Inspector, a MaterialSet slot, the Environment panel, or exposure
  adds **one** entry per gesture, never one per coalesced tick.
- Each in-scope discrete edit (rename, reparent, add/remove component, asset assign, render
  toggle, script override, env/atmosphere Select/Switch) adds exactly one entry with a
  sensible `humanizeFieldName`-style label and the correct `selectionId`.
- Entity create/duplicate/instantiate adds one undo-only entry (`redoable === false`, `undo`
  destroys the new id).
- **No out-of-scope op records anything:** a selection click, a gizmo-mode key, play
  transport, animation preview, an editor-camera move, and every Assets-panel /
  material-asset / filesystem mutation leave `past` untouched.
- The edits still behave exactly as before — coalescing, optimism, the poll, the toasts.
  Recording is additive: stripping every `pushEdit` / `commit` would leave behaviour
  identical.

e2e reach: `tests/e2e` drives the **engine** over the wire and never mounts the editor, so
this phase's recording logic is not directly e2e-testable. The inverse *commands* it replays
are already exercised by the engine suites (set-transform, set-material, rename, set-parent,
…); the one reachable editor-level assertion (an undo round-trips engine state) is added in
phase 06 once Ctrl+Z is wired.

## Notes / gotchas

- **Read the gizmo's new transform once, after `end`, with a single `inspect` — never an
  `inspect` per drag tick.** The viewport streams NDC and the authoritative transform is
  committed engine-side on `end`; one off-hot-path read is correct and cheap, a per-tick read
  would flood the serialized socket.
- **`smooth` is a transport detail, not part of the recorded value.** Mid-drag sends animate
  toward the value; a replay must send the exact value with no `smooth` so undo/redo snap —
  matching how `onFieldDragEnd` already does the final exact re-push.
- **Capture the prior value before the optimistic overlay.** `applyOptimisticComponent`
  mutates the live `componentsBySelected` DTO, so grab (and structurally copy) the prior at
  `onDragStart` / before `onFieldChange`'s overlay / before `applyOptimisticEntityName` —
  reading it afterward gives the already-changed value.
- **Reparent records in the store action, not the panel, and only on the success path.** The
  rejected path already rolls back and never bumps sceneVersion, so recording there would
  push an inverse for an edit that never happened.
- **Guard every recording path with the `historyReplaying` check** so an undo does not push
  its own inverse as a fresh entry. The guard suppresses the *recording*, not the control
  call.
- **Distinguish a scrub gesture from a discrete typed edit with the gesture ref**, not by
  comparing values. The ref is set at `onDragStart` and cleared at `onFieldDragEnd`;
  `onFieldChange` records only when it is null.
- The MaterialSet and generic-component paths both ultimately call `setComponent` /
  `setMaterial`; reuse `sendWrite`'s routing for the inverse rather than re-deriving the
  command per edit, so a future component lands in the inverse path automatically.
