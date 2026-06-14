# Phase 4 — Collision layers + sensors/triggers + contact events

**Status:** COMPLETED

## Goal

Make collisions *selective* and *observable*. Add a fixed `ObjectLayer`/`BroadPhaseLayer` set + a built-in
collision matrix that `RigidbodyComponent.layer` indexes; implement the three Jolt filter interfaces that
matrix drives; let a `ColliderComponent` be a **sensor** (`isSensor` → a Jolt body that overlaps but does
not solve); and wire a `ContactListener` that drains overlap/contact begin/end into a bounded, seq-cursored
**event ring** on the `PhysicsWorld`. That ring is drained over the control plane (mirroring `drain-alarms`)
and surfaced to scripts as a gameplay callback (mirroring how `tickScripts` already finds `on_update`). The
visible result: a dynamic body falls through a sensor volume and a script-visible *enter/exit* event fires —
the foundation gameplay (triggers, pickups, zones) is built on.

This phase does **not** add the character controller (Phase 5), raycasts (Phase 6), or anything ragdoll
(Phases 8-9). It is layers + filters + sensors + the contact drain, nothing more.

## What exists to build on

- **The `PhysicsWorld` + `RigidbodyComponent`/`ColliderComponent` from Phases 1-3.** Phase 1 stood up
  `Saffron.Physics` (classic `#include`, no `import std`, edge `→ {Core, Geometry, Scene, Animation}`, only
  `Saffron.Host` imports it) with `PhysicsWorld` wrapping Jolt's `PhysicsSystem` + `BodyInterface` +
  `TempAllocator` + `JobSystem` + the `entity ↔ BodyID` maps, owned by `HostState` as a sibling of
  `animation`/`script` (`host.cppm:53-54`), built on the `Edit→Playing` edge of `onPlayStateChanged`
  (`scene_edit_context.cppm:220`) and stepped inside the `simTick` seam (`scene_edit_play.cpp:198-203`).
  Phases 2-3 added `RigidbodyComponent` (motion type, mass, damping, gravity factor, locks, **`layer`**)
  and `ColliderComponent` (shape, dimensions, source mesh, local offset, `PhysicsMaterial{friction,
  restitution}`, **`isSensor`**) to `Saffron.Scene`. This phase gives `layer` and `isSensor` teeth.
- **`RigidbodyComponent.layer` is already a serialized field** — an index, declared in `scene.cppm` and
  registered via the component recipe (`scene/AGENTS.md`: declare in `scene.cppm`, serde body in
  `emitSceneSerde()` in `tools/gen-control-dto/gen.ts`, register once in
  `scene_edit_components.cpp:registerBuiltinComponents`). This phase fixes what that index *means* (a slot
  in the v1 layer enum) and consumes it in the filters; it adds **no** new scene component, only the
  physics-side enum + matrix that interpret an existing field.
- **`ColliderComponent.isSensor` is already a serialized bool** (same recipe). This phase makes it set
  `BodyCreationSettings.mIsSensor` (or `Body::SetIsSensor`) when the body is created in the world build.
- **The `drain-alarms` event-ring precedent — the closest analog.** `AlarmState` (`renderer_types.cppm:951`)
  holds an append-only, seq-stamped ring: `std::array<AlarmEvent, AlarmEventRingCapacity> events`
  (`:954`, `AlarmEventRingCapacity = 256` at `:890`), `eventHead`/`eventCount`, `nextSeq = 1` (`:957`).
  `pushAlarmEvent` (`renderer.cppm:1446`) stamps `seq` and advances the head; `drainAlarms` returns an
  `AlarmDrain { events, highWaterSeq, oldestSeq, overflowed }` (`renderer_types.cppm:941`) of events with
  `seq > since`. The command `drain-alarms {since}` (`control_commands_render.cpp:538`) is "non-blocking",
  cursored by `since`, and reports `overflowed` when the ring dropped past the caller's cursor. **The
  contact-event ring is this pattern, owned by `PhysicsWorld` instead of `AlarmState`.**
- **The `drain-script-errors` precedent — the script-surfacing half.** `ScriptError`
  (`scene_edit_context.cppm:161`) is a bounded ring (`ScriptErrorRingCap = 256`, `:170`) on
  `SceneEditContext` (`scriptErrors` `:227`, `scriptErrorSeq` `:228`); `pushScriptError`
  (`scene_edit_play.cpp:206`) stamps `seq` + the current `playTick`, evicting the oldest at cap (`:209-211`);
  `drain-script-errors {since}` (`control_commands_scene.cpp:1053-1078`) returns
  `DrainScriptErrorsResult { events, highWaterSeq, oldestSeq, overflowed }` (`control_dto.cppm:447`) with
  `overflowed = oldestSeq > 0 && since + 1 < oldestSeq` (`:1067`). The DTO `ScriptErrorDto`
  (`control_dto.cppm:433`) carries `seq`, a `WireUuid entity`, strings, and a `tick`. **The contact DTO
  mirrors this field-for-field**, swapping the message strings for the two entities + an event kind.
- **How a script callback is dispatched.** `tickScripts` (`script.cppm:103`) runs each instance's
  `on_update(self, dt)`, found via `lua_getfield(L, -1, "on_update")` (`script_runtime.cpp:280`) and called
  through `lua_pcall` under the traceback handler (`script_runtime.cpp:247`, `tracebackHandler` at `:167`).
  A failing instance returns a `ScriptRunError` (`script.cppm:74`) which the Host's `simTick`
  (`host.cppm:725-731`) routes to `pushScriptError`. **A contact handler is the same shape**: after the
  physics step, for each new contact event whose entity owns a `ScriptComponent`, look up `on_contact` /
  `on_trigger_enter` / `on_trigger_exit` the way `on_update` is looked up, `pcall` it under the same
  traceback handler, and route a failure to `pushScriptError`.
- **The simTick composition (`host.cppm:882-890`).** `tickAnimation` runs, then `tickPlay` runs `simTick`.
  The Host composes physics + scripts inside that one `simTick` closure (`host.cppm:719-732` is the v1
  script-only body this phase extends): step the world, write transforms back, **then** dispatch contact
  callbacks to scripts, **then** run `on_update` — all before `renderScene` the same frame. No new render
  pass.
- **The e2e drain harness.** `alarms.test.ts` drives the cursor loop: `drain-alarms {since:0}` → assert an
  event with a `firing` `state` and a `highWaterSeq` ≥ its `seq` (`alarms.test.ts:26-37`); re-drain from
  `highWaterSeq` and assert only newer events (`:48-53`); a tail drain re-sends nothing (`:62-65`).
  `rig-query.test.ts` is the recent control-command sibling to mirror for shape.

## Work

### 1. The fixed layer set + collision matrix (`Saffron.Physics:Types`)

Five object layers in v1, an enum on the physics side. `RigidbodyComponent.layer` (the serialized index)
maps onto it; an out-of-range index clamps to `Moving` with a logged note.

```cpp
/// Object-layer slots a body lives in. v1 is a fixed set; a project-authored matrix is deferred.
/// RigidbodyComponent.layer indexes this; a ColliderComponent with no Rigidbody is implicitly Static.
enum class ObjectLayer : u8
{
    Static,     // immovable world geometry (floors/walls): the implicit layer of a lone collider
    Moving,     // dynamic + kinematic bodies (the default for a Rigidbody)
    Character,  // the character controller's body (Phase 5 fills this in)
    Debris,     // dynamic bodies that collide with world/character but not each other (perf)
    Sensor,     // trigger volumes: overlap-only, never solved
    Count
};

/// Whether two object layers may collide. Symmetric. The whole v1 collision policy is this table —
/// no per-project authoring yet (deferred; see README "Explicitly OUT").
auto layersCollide(ObjectLayer a, ObjectLayer b) -> bool;
```

The matrix (v1, locked): `Static` collides with `Moving`/`Character`/`Debris` but not `Static`/`Sensor`;
`Moving` collides with everything except `Sensor`-vs-`Sensor`; `Debris` collides with `Static`/`Moving`/
`Character` but **not** `Debris`; `Sensor` overlaps every solid layer (to generate trigger events) but is
never solved against anything (it produces contacts, applies no impulse — that is the sensor body's job,
not the matrix's, but the matrix must still let the pair through so the `ContactListener` sees it).

Two broad-phase layers back this (Jolt's coarse AABB tier): `BpLayer::NonMoving` (Static) and
`BpLayer::Moving` (everything else). Keep it to two; more is a perf micro-opt not worth v1 complexity.

### 2. The three Jolt filter interfaces (impl `.cpp`, the only Jolt-including TU)

Jolt requires three implemented interfaces wired into `PhysicsSystem::Init`. They are pure functions of
the layer set above — no per-frame state — so they live as small structs in the impl unit, constructed once
when `PhysicsWorld` is built.

```cpp
// All three live in the single Jolt-including .cpp; none are exported.

// Object layer -> broad-phase layer (the coarse tier Jolt buckets AABBs into).
class BroadPhaseLayerImpl final : public JPH::BroadPhaseLayerInterface { /* GetNumBroadPhaseLayers, GetBroadPhaseLayer */ };

// "May an object in this layer collide with this broad-phase layer?" (the coarse cull).
class ObjectVsBroadPhaseImpl final : public JPH::ObjectVsBroadPhaseLayerFilter { /* ShouldCollide */ };

// "May these two object layers collide?" — defers to layersCollide().
class ObjectLayerPairImpl final : public JPH::ObjectLayerPairFilter
{
public:
    auto ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const -> bool override
    {
        return layersCollide(static_cast<ObjectLayer>(a), static_cast<ObjectLayer>(b));
    }
};
```

Hold instances of all three by value inside the `PhysicsWorld` (the impl-side concrete struct, not the
opaque handle in `:Types`); pass them to `PhysicsSystem::Init(maxBodies, …, broadPhase, objVsBp, objPair)`.
Jolt borrows them for the world's lifetime, so they must outlive every `Update` — owning them in the world
struct is exactly that.

### 3. Sensor bodies from `ColliderComponent.isSensor`

In the Phase-2/3 world-build loop that creates a Jolt body per `(Rigidbody?, Collider)` entity, when
`collider.isSensor` is set:

- Set `BodyCreationSettings.mIsSensor = true` (or `Body::SetIsSensor(true)` post-create).
- Put the body in `ObjectLayer::Sensor` regardless of any `RigidbodyComponent.layer` — a sensor is a sensor.
- A sensor with no `RigidbodyComponent` is **kinematic** (it has a transform to follow but no dynamics); a
  sensor *with* a dynamic `RigidbodyComponent` is unusual but legal (a "falling trigger") — keep its motion
  type, just flag it a sensor. The matrix (§1) already lets `Sensor` overlap solids without solving, so the
  `ContactListener` (§4) sees the overlap and emits an event but no impulse is applied.

No new field — `isSensor` already serializes (Phase 2). This step is purely the world-build branch that
reads it.

### 4. The `ContactListener` + the contact-event ring (the headline)

Implement `JPH::ContactListener` in the impl unit; its three callbacks drain into a bounded ring on the
`PhysicsWorld`. The ring is the `AlarmState` pattern (`renderer_types.cppm:951-957`), owned here.

```cpp
/// One contact/overlap transition, seq-stamped, drained over a non-blocking cursor.
/// Sensor overlaps and solid touches share one ring; `sensor` distinguishes them.
struct ContactEvent
{
    u64 seq = 0;
    enum class Kind : u8 { Begin, End } kind = Kind::Begin;  // OnContactAdded / OnContactRemoved
    u64 entityA = 0;   // IdComponent uuid of the lower BodyID's entity
    u64 entityB = 0;   // the other entity's uuid
    bool sensor = false;  // either body is a sensor (a trigger overlap, not a solid touch)
    glm::vec3 point{ 0.0f };   // a representative world contact point (manifold[0]); zero for an End event
    glm::vec3 normal{ 0.0f };  // world contact normal (A -> B); zero for an End event
    i64 tick = 0;              // the play tick the contact fired on (like ScriptError.tick)
};

inline constexpr std::size_t ContactEventRingCap = 256;
```

The listener is a problem: Jolt invokes `OnContactAdded`/`OnContactPersisted`/`OnContactRemoved` **from job
threads during `Update`**, so they must not touch the entity maps or push to the ring directly under a data
race. Follow Jolt's documented contract — buffer the raw `(BodyID, BodyID, manifold)` under a small mutex in
the listener, then **drain that buffer into the seq-stamped `ContactEvent` ring on the simulation thread
right after `Update` returns**, where the `BodyID → entity-uuid` map is safe to read. `OnContactPersisted`
is ignored for v1 (we emit Begin/End transitions only, not per-frame stay events — that is the trigger
model gameplay wants; a "still overlapping" stream is deferred).

```cpp
/// Append a contact transition to the world's ring, stamping seq + the current tick (evict oldest at cap).
void pushContactEvent(PhysicsWorld& world, ContactEvent event);

/// Snapshot of events with seq > since (non-blocking), plus the cursor metadata drain-alarms reports.
struct ContactDrain
{
    std::vector<ContactEvent> events;
    u64 highWaterSeq = 0;
    u64 oldestSeq = 0;
    bool overflowed = false;
};
auto drainContacts(const PhysicsWorld& world, u64 since) -> ContactDrain;
```

### 5. Dispatch contacts to scripts (in the Host `simTick` composition)

Extend the Host's `simTick` closure (the v1 body at `host.cppm:719-732`) so the composed order each tick is:
**step physics → write transforms back → drain *this tick's* new contact events → for each, dispatch to the
two entities' scripts → run `on_update`.** The script dispatch mirrors `on_update` lookup
(`script_runtime.cpp:280`): for a `Begin` sensor event call `on_trigger_enter(self, otherEntity)`, for an
`End` sensor event `on_trigger_exit(self, otherEntity)`, for a solid `Begin` `on_contact(self, otherEntity,
point, normal)` — each found via `lua_getfield`, `pcall`ed under `tracebackHandler` (`script_runtime.cpp:167`),
a failure routed to `pushScriptError` exactly as `tickScripts` failures are today (`host.cppm:725-731`). A
missing handler is a silent skip (most entities won't define one). The dispatch loop reads the contact ring
the same frame it was filled, but the ring also retains events for the control-plane drain (§6) — the two
consumers share one ring, cursored independently (scripts consume eagerly per tick, control consumes lazily
by `since`).

### 6. Control command + DTO + `se` verb

Add `drain-contacts {since}` mirroring `drain-alarms`/`drain-script-errors` exactly. New DTOs in
`control_dto.cppm` (then `bun run tools/gen-control-dto/gen.ts` regenerating all five outputs per
`control/AGENTS.md`):

```cpp
struct ContactEventDto
{
    i64 seq;
    ContactKindDto kind;  // "begin" | "end"
    WireUuid entityA;
    WireUuid entityB;
    bool sensor;
    Vec3Dto point;
    Vec3Dto normal;
    i64 tick;
};

struct DrainContactsParams { std::optional<i64> since; };

struct DrainContactsResult
{
    std::vector<ContactEventDto> events;
    i64 highWaterSeq;
    i64 oldestSeq;
    bool overflowed;
};
```

```cpp
registerCommand<DrainContactsParams, DrainContactsResult>(
    reg, "drain-contacts", "drain-contacts {since} — contact/trigger events with seq > since (non-blocking)",
    [](EngineContext& ctx, const DrainContactsParams& params) -> Result<DrainContactsResult>
    {
        // The PhysicsWorld lives on the Host; route through the control context handle the
        // physics commands use. In Edit (no world) return an empty drain, never an error.
        // overflowed = oldestSeq > 0 && since + 1 < oldestSeq  (the drain-script-errors rule).
    });
```

Empty-drain-in-Edit (no error) matches `drain-alarms`' non-blocking contract — the editor polls it
unconditionally on the reconcile loop. Add the `drain-contacts` verb to `tools/se` (the keep-scriptable
rule), formatting the events the way `se` formats the alarm/script-error drains.

### 7. Auto-fit a sensor's shape on add

Per the locked decision (auto-fit shapes on add), a `ColliderComponent` added with `isSensor` set still
fits its shape to the entity-mesh AABB on add — a sensor is not special at authoring time, only at
simulation time. No work here beyond confirming the Phase-2 auto-fit path runs regardless of `isSensor`
(it should already, since fitting reads the mesh, not the sensor flag).

## Validation (done criteria)

- `make engine` green: the layer enum + matrix + three filter interfaces + `ContactListener` + the ring
  compile; the impl `.cpp` remains the only TU including `<Jolt/…>`.
- `make prepare-for-commit` clean (clang-format + clang-tidy) over the new physics + control files.
- `bun run check` clean (the regenerated `@saffron/protocol` carries `ContactEventDto`/`DrainContactsResult`/
  `ContactKindDto` and typechecks).
- `make e2e`: a new `tests/e2e/physics-triggers.test.ts` (mirroring `alarms.test.ts`' cursor loop + the
  `rig-query.test.ts` boot/import shape): spawn a dynamic body above a `Sensor` collider, `enter-play`,
  settle a few ticks, `drain-contacts {since:0}` and assert a `begin` event for the pair with `sensor:true`,
  then (after it falls clear) a re-drain from `highWaterSeq` yields the matching `end`; assert a tail drain
  re-sends nothing and the validation log is clean. A second case asserts two `Debris`-layer bodies do **not**
  generate a contact (the matrix excludes `Debris`-vs-`Debris`) while each still contacts the `Static` floor.
- `docs/`: update `docs/content/explanations/physics/` — add a **collision layers, sensors, and contact
  events** page (the fixed layer set + the v1 matrix table, the sensor model, the contact-event ring + its
  two consumers: the `drain-contacts` control drain and the `on_trigger_enter`/`on_contact` script
  callbacks), and add its row to the physics hub `_index.md`. Cite UE5 (collision channels + object/trace
  responses, the overlap-vs-block model, `OnComponentBeginOverlap`) and Unity (layer collision matrix,
  `OnTriggerEnter`/`OnCollisionEnter`) as the reference points the v1 set deliberately simplifies.

## Notes / gotchas

- **The listener runs on Jolt job threads.** `OnContactAdded`/`Removed` fire concurrently during `Update`;
  never touch the `entity ↔ BodyID` maps or push the seq-stamped ring from inside them. Buffer raw body-pair
  data under the listener's own mutex, drain it to the `ContactEvent` ring **after** `Update` returns on the
  sim thread (where the maps are single-threaded-safe). Getting this wrong is a heisenbug, not a crash.
- **Seq is monotonic for the whole play session, the ring is per-world.** The `PhysicsWorld` (and its ring)
  is built on `Edit→Playing` and discarded on `→Edit` (the play-duplicate-dies-on-stop model,
  `scene_edit_play.cpp` / `scene_edit_context.cppm:215`), so each play session starts a fresh ring at
  `seq = 1`. An editor cursor held across a stop/start must reset — surface `oldestSeq` so a stale `since`
  reports `overflowed` and the editor resyncs, exactly as `drain-script-errors` does (`:1065-1067`).
- **A lone `ColliderComponent` is `ObjectLayer::Static`** (the locked split rule) — the world-build must
  derive the layer from `RigidbodyComponent.layer` *only when a Rigidbody is present*, falling back to
  `Static` otherwise, and overriding to `Sensor` whenever `isSensor` is set. Three sources, one resolved
  layer; document the precedence (`isSensor` > `Rigidbody.layer` > implicit `Static`) inline where it's read.
- **`Sensor` must pass the broad/narrow phase to be seen.** A trigger generates contacts only if the pair
  survives both filters; the matrix (§1) must let `Sensor` overlap every solid layer. The "no impulse"
  behavior comes from the Jolt sensor flag (`mIsSensor`), **not** from the matrix excluding the pair —
  excluding it would also suppress the contact event. Keep that distinction straight (it is the #1 sensor bug).
- **Do not stream `OnContactPersisted`.** v1 emits Begin/End transitions only — that is the trigger model.
  A per-frame "still overlapping" stream (and contact-force readback) is deferred; the ring's `Kind` enum
  leaves room for it (`Begin`/`End` today, a `Stay` later) without a wire break.
- **Deferred (say so):** a project-authored collision matrix (v1 is the fixed enum), per-channel trace
  responses, contact impulse/force in the event payload, continuous-collision (CCD) tuning, and the
  character controller's use of the `Character` layer (Phase 5). Each extends this same layer/filter/ring
  machinery, not a rewrite of it.
