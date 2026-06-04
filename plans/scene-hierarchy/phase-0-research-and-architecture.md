# Phase 0: Research and architecture

**Status:** NOT STARTED

<!-- Flip to COMPLETED when the decisions below are ratified and the phase map is current; delete this file only after COMPLETED + merged. -->

## Goal

Pin the architecture before any code lands, so phases 1..7 reference one source of truth instead of re-litigating shape decisions. The engine is flat today: an `Entity` is a bare `entt::entity` handle (scene.cppm:236), `createEntity` mints only Id/Name/Transform (scene.cppm:272-279), `TransformComponent` is local-treated-as-world (scene.cppm:38-43), and there is no parent/child data anywhere — `sceneFromJson` even carries a dormant uuid→handle hook with the comment "No reference-holding components exist yet; the hook is ready for them" (scene.cppm:622-624). This document surveys how other engines model hierarchy, then records each decision (recommendation + rationale + rejected alternatives) and the concrete C++ data model that phases 1-2 build.

## How other engines model this

| Engine | Tree node | Components in tree? | Reparent semantics | Environment/sky | Skeleton |
|--------|-----------|---------------------|--------------------|-----------------|----------|
| **Unreal** | Actor (Outliner); editor-only Folders create no runtime parent | Yes — per-actor SCS component tree (`USCS_Node`) under each Actor's `RootComponent` | `AttachToActor` / `AttachToComponent`; `UpdateComponentToWorld` propagates | Sky as **Actors** (DirectionalLight, SkyAtmosphere, SkyLight, SunSky) in the Outliner | Inside the `USkeleton` **asset**, not the Outliner; sockets attach by bone name |
| **Unity** | GameObject; empty GameObject is the idiomatic grouping parent (real, transformable) | No — components are a **flat Inspector list** for the selected object | `Transform.SetParent(p, worldPositionStays=true)`: keep world, recompute local | **Global** `RenderSettings.skybox` / ambient (Lighting window); Reflection/Light Probes **are** GameObjects | Bones **are** real GameObjects; `SkinnedMeshRenderer.bones` is an ordered `Transform[]`, `rootBone` the root |
| **Godot** | Everything-is-a-Node; the Scene dock **is** the tree | n/a — nodes, not components | drag in the Scene dock | `WorldEnvironment` **node** (deletable) holding an Environment + Sky resource | `Skeleton3D` node holds bones; `BoneAttachment3D` proxies a transform |
| **Bevy** | Entity with `ChildOf` (parent) + `Children` (auto-synced) | n/a — pure ECS | despawn takes descendants; `GlobalTransform` derives from `Transform` once per frame in PostUpdate | n/a (resource/component) | parent bone entities into the tree |
| **glTF (import truth)** | `scene` = forest of root node indices; each `node` has a `children[]` + local matrix or TRS | n/a | n/a | n/a | `skin`: `joints` = node indices, `inverseBindMatrices` accessor; skinned-mesh node transform ignored |

The contrasts that drive our decisions:

- **Components in the tree (Unreal) vs. in the Inspector (Unity).** Unity's split — tree nodes are GameObjects, components are a flat Inspector list — maps 1:1 onto an entt engine (entity == GameObject, entt component == Inspector row). Unreal's per-actor SCS tree needs a per-actor component graph the registry-driven design does not have.
- **Parenting as data on one component (Unity's `Transform.parent`, Bevy's `ChildOf`) vs. a structural payload.** Both store the parent link as a field, not as a nested graph the server ships. The client builds the tree.
- **Environment as a node (Godot `WorldEnvironment`) vs. global settings (Unity `RenderSettings`).** Godot gives discoverability (the env is selectable in the tree); Unity gives safety (the sky has no transform, is not picked, is singleton). A pinned non-deletable Environment node — visible like Godot, untransformed/singleton like Unity — is the discoverable-and-safe middle for a single-scene editor.
- **Bones as scene nodes (Unity/Godot/Bevy) vs. asset-internal (Unreal).** For an entt engine, bones-as-entities is the lower-friction model and maps 1:1 to glTF (one entity per node, joints are node indices) — exactly the import path the hierarchy already needs. Unreal's asset-internal skeleton requires an asset-side transform system Saffron lacks.
- **Bevy is the closest analog.** `ChildOf`/`Children` + a once-per-frame `GlobalTransform` derivation feeding culling/instancing is precisely the substrate this plan builds in entt.

## Decisions

### 1. Components stay in the Inspector (Unity model), not the tree (Unreal)

**Decision.** Tree nodes are **entities only**. A selected entity's components remain a flat list in the data-driven `InspectorPanel` (driven by `inspect`). As an optional v1.1 nicety behind a toggle, render the *selected* entity's components as read-only leaf subrows derived from the already-fetched `componentsBySelected` (zero extra control calls); clicking one selects the entity and focuses that component in the Inspector. Full per-entity component subrows (Unreal SCS tree) are out of scope.

**Why.** Unity proves the clean split for an ECS: `entt` entity == GameObject, `entt` components == the Inspector list. The Inspector is already switch-free and reflection-driven, so it needs no change. Unreal's per-actor component subrows require N `inspect` calls + an `(id, sceneVersion)` cache, which fights the focus-gated, version-keyed ~6 Hz reconcile poll that today inspects only the selected entity (store.ts:257). The selected-only subrow variant respects that poll because `componentsBySelected` is already in the store.

**Rejected.** Unreal-style per-entity component subrows in the tree — needs N inspects + a cache, fights the "poll, only on version change, only the selected entity" design, and clutters the outliner. Recorded as a deferred option (Phase 6's full variant), not a v1 phase.

### 2. Environment stays global `SceneEnvironment`, surfaced as a pinned virtual node

**Decision.** Keep `SceneEnvironment` as global `Scene` state (scene.cppm:226-231); do **not** promote it to an entity/component. Surface it in the outliner as a **pinned, non-deletable, non-reparentable virtual "Environment" node** at the top of the tree. The node is a client-side sentinel in the React tree — **not** a real `EntityRef`, **not** a real entity — whose selection switches the bottom tab to the existing `EnvironmentPanel` (or focuses env fields in the Inspector). Engine-side this is **zero** restructuring: `list-entities` stays entity-only; the editor injects the synthetic node. Reflection probes / GI volumes, when they come, *are* real entities with components (they have a transform + a zone).

**Why.** This combines Godot's discoverability (the env is visible and selectable in the tree — answering the user's "in tree?" as yes) with Unity's safety (no transform, not picked, singleton). The skybox plan is settled here: `plans/skybox/phase-1` is COMPLETED and the skybox README's Recommendation explicitly rejects modeling sky as a mesh entity, keeping `SceneEnvironment` as global Scene state and reserving entity/component form only for future *placed* objects (`SkyAtmosphereComponent`, `ReflectionProbeComponent`). Promoting it to a real entity re-opens that settled decision and relocates `environmentToJson`/`FromJson`, the `get`/`set-environment` commands, and the three renderer resolve sites (IBL bake assets.cppm:924-947, ambient/DDGI assets.cppm:903-918, visible-sky assets.cppm:958-978). A client-side sentinel honors the tree request with no engine churn and no scene-file migration — the change is purely additive in the editor.

**Rejected.** Promote to a singleton `EnvironmentComponent` on a non-deletable entity (full Godot `WorldEnvironment`) — contradicts the COMPLETED skybox plan, relocates serde + three renderer resolve sites + the control commands, and gains nothing for a single, global, untransformed env. Leave env **only** in the `EnvironmentPanel` tab (pure Unity, no tree node) — the user asked whether the skybox belongs in the tree; a pinned sentinel answers yes cheaply and matches a scene-wide "World" at the root. Make sky a giant unlit mesh entity — rejected by the skybox README (fights depth prepass, picking, IBL).

### 3. Relationship-component shape: durable parent Uuid + non-serialized caches

**Decision.** One `RelationshipComponent` per entity holding a durable **parent `Uuid`** (`0` == root) plus non-serialized runtime caches: a resolved `parentHandle` and a `children` vector of live handles, rebuilt by a `relinkHierarchy(Scene&)` pass after every structural change. Serialize/copy **only** the parent `Uuid`; never the handles. Siblings are unordered in v1 (`push_back`; entt/insertion order). Registered as a non-removable component, filtered out of the Inspector.

**Why.** entt bakes in no hierarchy — you add a relationship component (skypjack). A durable parent uuid is the minimum that survives reload, because `entt::entity` (index+version) is not stable across load and the scene file is uuid-keyed (like `MeshComponent`, scene_edit_components.cpp:46). The `children`/`parentHandle` caches make tree traversal, the world-transform flatten pass, reparent, and recursive-destroy O(children) instead of O(N) scans. Storing the caches as **non-serialized, non-copied** is the load-bearing safety choice: `registerComponent`'s `copyTo` emplaces a value copy (scene.cppm:465) and copy-entity does a naive per-component copy (control_commands_scene.cpp:533), so a serialized/copied children-of-handles field would alias the source or emit non-portable entt ids. A parent-uuid-only durable surface keeps the generic reflection machinery correct with zero special-casing. A `{parent uuid; children vector}` is the ergonomic middle the research calls out — heavier than skypjack's intrusive linked list but right for an editor-scale tree.

**Rejected.** A bare parent field with on-demand child scans — children-of queries and the per-frame world pass become O(N) (or O(N²) re-resolving uuids), with nowhere to cache the resolved handle. Full skypjack relationship struct (`firstChild`/`prev`/`next`/`childCount`, no vector) — zero-alloc and O(1) mid-removal but children are unpacked in memory and it is more code than an editor tree needs; revisit if profiling demands it. Storing children/parent as **live handles in the serialized form** — entt ids are not stable across load and the value-copy hazard corrupts copies.

### 4. World transform: a single per-frame flatten pass, not recompute-on-read

**Decision.** One `updateWorldTransforms(Scene&)` flatten pass runs **once per frame before render**, writing a cached `WorldTransformComponent{mat4}` on every transformable entity in **parent-before-child order** (walk roots → children via the `RelationshipComponent` caches, not `forEach` — entt views are unordered). All consumers — mesh draw (assets.cppm:822), pick (assets.cppm:1019), point/spot light position (assets.cppm:727-766), `primaryCamera` (scene.cppm:311), the gizmo, billboards — read the cache via `worldMatrix`/`worldTranslation`/`worldRotation`. `transformMatrix` (scene.cppm:105) stays the **local** builder, unchanged.

**Why.** The research is explicit: one resolve pass writes cached world matrices in parent-before-child order, then the renderer iterates the flat world-matrix pool linearly with no hierarchy walk (Bevy derives `GlobalTransform` once per frame in PostUpdate feeding culling/instancing). Recompute-on-read `worldMatrix()` is simplest but O(depth) **per consumer** across the mesh loop, pick loop, billboards, and lights — and re-resolving parent uuids per call via the O(n) `resolveEntity` scan (control_server.cpp:72) would be O(N²). A full dirty-flag scheme (recompute only dirtied subtrees) is the perf endgame but adds bookkeeping the editor does not yet need; one unconditional pass over an editor-scale scene is one pass/frame. The cache is a derived (unserialized) component — like `IdComponent` it is unregistered, so `serializeEntity` auto-skips it (scene.cppm:526) and it never pollutes the scene file. Full `mat4` composition preserves non-uniform parent scale, so `normalMatrix = transpose(inverse(mat3(world)))` stays correct.

**Rejected.** Recompute-on-read `worldMatrix()` walking ancestors per consumer — O(N·depth)/frame across 4+ consumer loops, risks O(N²) via uuid re-resolution; kept only as the `worldMatrix()` fallback when the cache is stale. Scene-owned `unordered_map<entt::entity, mat4>` worldCache — viable, but a component co-locates with the entity, iterates with entt, and is auto-skipped by `serializeEntity`; preferred. Dirty-flag incremental propagation — deferred; correct once scenes are large or gizmo drags dominate, unnecessary for v1.

### 5. Tree data flow: flat list + `parentId`, engine-authoritative reparent

**Decision.** The server stays the source of truth for parent links but sends a **flat list with a `parentId` per entry**; the React client builds the tree. Add an **optional** `parentId` (`$ref` uuid, absent/null == root) to the **list-entry type only** — not the shared `entity-ref` returned by ~12 commands. Reparent is engine-authoritative via a `set-parent {entity, parent?}` command. The editor injects the synthetic Environment node client-side.

**Why.** A flat array + `parentId` keeps ids as strings, keeps the `sceneVersion`-keyed diffing cheap, reuses the existing reconcile poll, and avoids deep-recursion schemas and the harder selection reconcile a nested payload forces (Unity holds parenting as data on one component, not a structural payload). Putting `parentId` on the **list-entry** type rather than the shared `entity-ref` is the low-blast-radius choice: `entity-ref` (control_server.cpp:134) is returned by ~12 commands and is `additionalProperties:false`, so widening it ripples everywhere. Reparent must be engine-side because preserving world position is transform math — `decompose(inverse(parentWorld) * childWorld)` — that belongs next to `TransformComponent`, and the world-transform machinery lands engine-side anyway. The editor has no world-matrix math.

**Rejected.** Server sends a nested children tree in `list-entities` — complicates the version-keyed poll and selection reconcile, fights id-as-string, forces a recursive schema. A separate `get-hierarchy` command — an extra fan-out call per poll for data that fits one optional field on the list already fetched. Client-side reparent math — the editor lacks world-matrix math; world-preservation is engine-authoritative.

### 6. Recursive destroy, cycle prevention, reparent math

**Decision.** `destroyEntity` becomes **recursive**: collect all descendant handles via the `children` cache **before** any `registry.destroy` (which invalidates handles), detach the entity from its parent's `children`, then destroy descendants bottom-up. `setParent(Scene&, child, newParent, keepWorld=true)` walks `newParent`'s `parentHandle` chain and returns `Err` if it reaches `child` (cycle) or if self-parenting; on success it splices the child between the old and new parents' `children`, sets the durable parent uuid, and (default `keepWorld`) rebases the child's **local** TRS so its world transform is unchanged: `child.local = decompose(inverse(worldMatrix(newParent)) * worldMatrix(child))`. The destroy command (control_commands_scene.cpp:84) must clear `sceneEdit.selected` if **any** destroyed descendant was selected, not just the root (today control_commands_scene.cpp:93 checks only the root).

**Why.** `registry.destroy` is non-recursive (skypjack) and invalidates handles, so descendants must be buffered first or they dangle as orphan roots. A cycle guard is mandatory: without an ancestor check, `setParent` can create a cycle that makes both the world-transform pass and recursive destroy infinite-loop. World-preservation on reparent is the editor convention (Unity `SetParent(worldPositionStays=true)`); the decompose is TRS-only and lossy under sheared parents (accepted — `TransformComponent` stores Euler+vec3 scale, no shear). Both drag-begin paths (host SDL host.cppm:444-448, control gizmo-pointer control_commands_scene.cpp:673-683) snapshot the entity transform and must adopt identical world↔local semantics or they diverge.

**Rejected.** Reparent orphaned children to root on destroy (the alternative to recursive destroy) — surprising in an editor; "delete subtree" is the expected gesture. Keep-local reparent (object jumps into parent space) — not the editor default; offered only as the `keepWorld=false` flag.

### 7. Serialization by uuid, two-pass resolve, SceneVersion 3

**Decision.** Register `RelationshipComponent` so `serializeEntity`/`deserializeEntity` pick it up; `toJson` emits only `{ "parent": uuid }`, `fromJson` reads only `Uuid{jsonU64Or(j,"parent",0)}` and leaves caches default. Bump `SceneVersion` 2 → 3 (scene.cppm:425), extend the version-history comment (scene.cppm:423), and the `sceneFromJson` upper-bound check (scene.cppm:579). Replace the dormant `static_cast<void>(uuidToHandle)` hook (scene.cppm:622-624) with the parent-resolution pass: `forEach<RelationshipComponent>`, map each `parent.value` through `uuidToHandle` to set `parentHandle` and push into the parent's `children`; a dangling parent uuid → treat as root + `logWarn`. A v1/v2 doc has no `RelationshipComponent`, so the post-load pass defaults any entity missing one to root (parent 0) — old scenes migrate clean.

**Why.** This is the documented two-pass loader's reason to exist: parent uuids may reference entities created later in the array, and `sceneToJson`'s `forEach<IdComponent>` (scene.cppm:561) emits entities in arbitrary entt order. Resolving after all entities exist is mandatory; the resolution pass is exactly `relinkHierarchy` specialized to the loader's map. Serializing only the parent uuid keeps the generic reflection machinery correct (no live handles in the file). Copy semantics (copy-entity control_commands_scene.cpp:533): the copied `RelationshipComponent` carries the same parent uuid — the copy joins the source's parent, correct for single-entity copy — and because the caches are non-serialized, the value-copy never duplicates live handles; just `relinkHierarchy` after the copy so the new entity appears in its parent's `children`. v1 is single-entity copy (no subtree recursion).

**Rejected.** Single-pass deserialize — fails when a parent appears later in the array. Storing parent as an entt handle in the file — dangles across reload. Subtree-recursive copy-entity (mint new uuids, remap parents) — a larger change deferred past v1.

## Recommended data model

All in `engine/source/saffron/scene/scene.cppm`, namespace `se`.

1. `RelationshipComponent` — the only parenting state. Place near `TransformComponent` (~scene.cppm:43):

   ```cpp
   struct RelationshipComponent
   {
       Uuid parent;                              // 0 == root; the ONLY serialized/durable field
       entt::entity parentHandle = entt::null;   // resolved cache — NOT serialized, NOT copied
       std::vector<entt::entity> children;       // derived cache — NOT serialized, NOT copied
   };
   ```

   Only `parent` is durable; `parentHandle`/`children` are rebuilt by `relinkHierarchy(Scene&)` from the parent uuids (via the loader's `uuidToHandle` at scene.cppm:592/609, or a fresh scan over `forEach<IdComponent>`). The registered `toJson` emits only `{ "parent": uuidToJson(c.parent.value) }`; `fromJson` reads only `Uuid{jsonU64Or(j,"parent",0)}`. `createEntity` (scene.cppm:272) emplaces a default `RelationshipComponent{parent:0}` alongside Id/Name/Transform so every entity is uniformly hierarchy-addressable (root by default). Mark it **non-removable** in `registerBuiltinComponents` and filter it out of `InspectorPanel` (parenting is edited via the tree / `set-parent`, not a raw uuid field).

2. Cached world transform — the flatten pass, not recompute-on-read:

   ```cpp
   struct WorldTransformComponent { glm::mat4 matrix{ 1.0f }; };  // derived, overwritten each frame

   void updateWorldTransforms(Scene& scene);          // one pass/frame, roots first then BFS/DFS
   auto worldMatrix(Scene&, Entity) -> glm::mat4;      // reads the cached WorldTransformComponent
   auto worldTranslation(Scene&, Entity) -> glm::vec3; // = vec3(worldMatrix[3])
   auto worldRotation(Scene&, Entity) -> glm::quat;    // for gizmo Local-space axes + spot/cam aiming
   ```

   `updateWorldTransforms`: for each root (`parentHandle == entt::null`), `world = transformMatrix(local)`; for each child in order, `world = parent.WorldTransformComponent.matrix * transformMatrix(local)`. `WorldTransformComponent` is **unregistered** (like `IdComponent`) so `serializeEntity` skips it (scene.cppm:526).

3. Reparent + destroy (free functions near `destroyEntity`, scene.cppm:281):

   ```cpp
   auto setParent(Scene&, Entity child, Entity newParent, bool keepWorld = true) -> Result<void>;
   void destroyEntity(Scene&, Entity);  // recursive: collect descendants via caches, destroy bottom-up
   ```

   `setParent`: cycle guard (walk `newParent`'s `parentHandle` chain, `Err` if it reaches `child`; self-parent `Err`); detach from the old parent's `children`, splice into the new (`push_back`, unordered v1); set the durable parent uuid (`entt::null`/`0` → root); when `keepWorld` (default, editor convention) capture `W = worldMatrix(child)` and set the child local TRS from `decompose(inverse(worldMatrix(newParent)) * W)`; `relinkHierarchy`; the next `updateWorldTransforms` recomputes.

## Phase map

| # | Phase | Scope (one line) |
|---|-------|------------------|
| 1 | Relationship component + cached world-transform propagation | Add `RelationshipComponent` (durable parent uuid + non-serialized caches), the per-frame `updateWorldTransforms` flatten pass + `WorldTransformComponent`, cycle-guarded world-preserving `setParent`, and recursive `destroyEntity` — parenting *exists* in the engine, consumers not yet rewired. |
| 2 | Serialize parent by uuid, two-pass resolve, SceneVersion 3 migration | Register `RelationshipComponent` (parent uuid only), turn the dormant `uuidToHandle` hook into the parent-resolution pass, bump SceneVersion 2→3, migrate v1/v2 (no relationship → root). |
| 3 | Adopt world transform across renderer, picking, camera, gizmo, billboards | Switch every transform consumer from local `transformMatrix` to the cached world matrix — in lockstep so visuals and picks never diverge. |
| 4 | Control plane: `parentId` on list-entities, `set-parent`, schema + contract test + se CLI | Emit optional `parentId` per list-entry, add cycle-guarded `set-parent` (clear via `parent=0`), author schemas, widen the contract test's u64 invariant, keep the se CLI usable — schema-first. |
| 5 | Editor tree-view outliner with drag-reparent + pinned Environment node | Replace the flat `HierarchyPanel` with a client-built tree (twisties, indentation, drag-to-reparent → `set-parent`), a non-deletable pinned Environment sentinel, expand-state kept out of the version-gated poll. Supersedes the typescript-ui phase-5 flat-list non-goal. |
| 6 | Optional: selected-entity component subrows in the tree | Behind a toggle, show the *selected* entity's components as read-only leaf subrows from `componentsBySelected` (no new control calls); click focuses the Inspector. Full per-entity subrows stay out of scope. |
| 7 | Forward-looking: glTF skin import + bones-as-entities + skinning pass (research-gated) | Skeletons as real bone entities parented into the tree + a `SkinnedMeshComponent { Uuid rootBone; std::vector<Uuid> bones; }`; glTF skins imported 1:1 as node entities; the GPU skinning pass. Depends on phases 1-3 but does **not** block them. |

Phases 1-2 are the load-bearing engine core; 3 makes parenting visible; 4-5 are the wire + editor; 6-7 are optional/forward-looking.

## Open questions / future

- **Skeleton representation (decided, build deferred).** A skeleton is real entt entities — one entity per bone with a `TransformComponent` + `RelationshipComponent`, parented into the same tree, plus a future `SkinnedMeshComponent { Uuid rootBone; std::vector<Uuid> bones; }` holding the ordered joint list by uuid. Bones are tree nodes (filterable behind a "hide bones" toggle). This is the Bevy/Unity/Godot precedent and maps 1:1 to glTF (entity per node, joints are node indices, `inverseBindMatrices` ride the skin — the same import path the hierarchy needs). **Skinning is unbuilt** (no skinning pass, no glTF skin import, no bind matrices), so Phase 7 is research-gated and forward-looking: the generic `RelationshipComponent` + world-transform machinery is exactly the substrate a skeleton animates against, so shipping it now is the prerequisite, not a dependency. *Rejected:* asset-internal skeleton (Unreal `USKeleton`) — no asset-side transform system, breaks "everything placeable is an entity"; a single opaque component holding a local-matrix bone array — cannot be inspected/attached-to in the tree and duplicates the transform-propagation logic, though it stays a valid optimization if per-bone entities prove too heavy.
- **Sibling ordering.** v1 is unordered (`push_back` / entt order; `list-entities` has no stable order today). A future explicit `order` index + `move-before`/`move-after` commands enable drag-to-reorder within a parent.
- **Dirty-flag world transforms.** A single unconditional flatten pass is the v1; an incremental dirty-flag scheme that recomputes only dirtied subtrees is the optimization once scenes are large or gizmo drags dominate.
- **Subtree copy-entity.** v1 copy joins the source's parent (single entity). Deep subtree duplication (recurse children, mint new uuids, remap parents) is a larger, later change.
- **Undo/redo and multi-scene.** Reparent/destroy should become reversible once undo/redo lands (AGENTS "Not yet"); a flat single-scene registry is assumed throughout — multi-scene is out of scope.
- **Folders.** Empty transform-only entities are the grouping nodes (Unity's idiom), not Unreal-style editor-only folders; non-transform folders could be a later, separate concern.
