# Phase 3 — alarms & event delivery

**Status:** COMPLETED

> The non-blocking delivery channel is fully in: a 256-entry seq-stamped event ring, the
> fingerprint-keyed active set, FIRING/RESOLVED coalescing (one FIRING on entry, one more
> on a severity escalation, one RESOLVED on the hysteresis exit), overflow reporting, and
> the `drain-alarms { since }` / `list-active-alarms` commands + `se` verbs. Detectors:
> `frame-budget` (EMA + hysteresis + debounce, warning→critical escalation), `frame-hitch`
> (MAD modified-z), `burn-rate` (short+long SLI), `vram` (threshold), `pso-compile` (info) —
> all reading the shared Phase-2 `PerfConfig`. **Deferred:** per-pass *drift / regression*
> (needs a per-pass baseline captured on scene load); the whole-frame sustained-regression
> case is already covered by frame-budget + burn-rate.

Notify the moment performance degrades, without spamming and without stalling the render loop. This
phase adds the detection math (smoothing, hysteresis, debounce, robust spike + burn-rate detectors),
a severity model, and — the architecturally load-bearing part — a **non-blocking delivery channel**
that fits the existing per-frame request/response control plane.

## Detection: smooth, then gate

Run detectors on a *smoothed* series, never raw per-frame values (those cross any threshold
constantly). Inputs are Phase 1/2's per-frame CPU/GPU/VRAM signals.

- **Irregular-interval EMA.** `alpha = 1 − exp(−dt / tau)`, then `ema += alpha * (sample − ema)`.
  Use `tau ≈ 300 ms` for the sustained-degradation gate and `tau ≈ 100 ms` for the hitch path
  (two EMAs over the same signal).
- **Hysteresis (deadband).** Separate enter/exit thresholds so the alarm doesn't chatter at the
  boundary. Frame-time example against the Phase 2 budget: **enter** when smoothed frame time
  `> 1.2 × budget` (20 ms @ 60 Hz), **exit** when `< 1.0 × budget` (16.67 ms).
- **Debounce ("for" duration).** Require the condition to hold ~250–500 ms (≈ 15–30 frames @ 60 Hz)
  before firing. Orthogonal to hysteresis: hysteresis stops edge oscillation, debounce stops
  one-frame breaches from firing at all.

## Detection: three detectors → severity

- **Hitch / spike (robust).** Modified z-score over a rolling window from Phase 2's ring:
  `mod_z = 0.6745 × (x − median) / MAD`, where `MAD = median(|xᵢ − median|)`; fire at
  `mod_z > 3.5`. MAD/median beat mean/stddev because the outlier you're hunting inflates stddev and
  masks itself; guard `MAD == 0` with a small floor. → **warning**.
- **Drift / regression.** Compare the current window median to a baseline captured on scene load (or
  a long EMA): `current / baseline − 1`; warn at +15–25%, critical at +50–100%. Add a slope test
  for VRAM (`> ~1 MB/s sustained for 10 s` → leak warning). Catches "this pass got 30% slower after
  that asset change" even while still under budget.
- **Sustained user-pain (burn-rate).** SLI = fraction of frames over budget over a window; require a
  **short and a long window to both breach** (fast detect + low false-positive, and it clears fast
  when the problem stops). Scaled to a frame loop: long 60 s / short 5 s → warning; long 10 s /
  short 1 s → critical.

Severity mapping:

| Severity | Example conditions | UX (Phase 4) |
|---|---|---|
| info | single PSO-compile hitch (`pipelinesCreated > 0`), TAA reset | alarm log only |
| warning | z-score hitch; in-deadband for the debounce; per-pass +15–25% regression; short-window burn breach | throttled toast + highlight the offending pass row |
| critical | sustained < 30 FPS (> 33.3 ms) for > 0.5 s; both burn windows breach; VRAM over budget; device-lost / validation error | persistent log entry + active-alarms badge |

All thresholds come from the Phase 2 `PerfConfig` so they stay data-driven and shared.

## Delivery: non-blocking poll, not a blocking long-poll

The control plane is pumped **synchronously, once per frame, on the render thread** (`pollControl`,
`host.cppm:525-529`; `drainControlServer`, `control_server.cpp`). A long-poll handler that *holds
the request open* until an event is ready would block **rendering** for the hold duration — wrong
here. The fit is the inverse:

- Detectors run each frame and **append** alarm events to a fixed-size engine-side **ring buffer**
  (~256 entries; at the editor's ~6 Hz drain that is ample headroom).
- A new **`drain-alarms`** command returns *immediately* with the queued events. Its handler only
  snapshots the ring — zero blocking, no render-thread stall. The editor calls it alongside
  `render-stats` in the existing reconcile loop.

### Cursor: never miss, never double-count

Stamp every event with a monotonic `seq` (u64). The client sends `drain-alarms { since: <lastSeq> }`
and gets events with `seq > since` plus the new high-water `seq`. This is SSE's `Last-Event-ID`
contract done over polling: a dropped/late poll just catches up next time (nothing lost), and
already-seen events are never re-sent. If the ring overflowed past `since`, return `overflowed:
true` and the oldest retained `seq` so the client resyncs from the active-alarm set instead of
silently losing events.

### Two structures

- **Active-alarm set**, keyed by `fingerprint = hash(metric + pass + severity)` — the current firing
  alarms; drives the editor badge and in-panel highlights. Returned by `list-active-alarms`.
- **Event log** (append-only, with `seq`) — FIRING/RESOLVED history; drives the alarm log and the
  `drain-alarms` cursor.

Coalesce repeats: while a fingerprint is already active, update its `count`/`lastSeen`/peak in place
— emit **one** FIRING on entry. When the metric recovers past the hysteresis **exit** threshold,
remove it from the active set and emit **one RESOLVED** event (same fingerprint, with duration +
peak) so the editor auto-clears the toast/highlight without a user dismiss. Optional inhibition:
suppress a per-pass warning when a whole-frame critical for the same frame is already firing.

### Why not push (for now)

SSE / WebSocket / a second async socket all buy lower latency than the ≤167 ms poll, but each needs
a new persistent connection and an off-thread send path the engine doesn't have. For an editor perf
monitor ≤167 ms is imperceptible, so the non-blocking events queue on the existing poll wins; keep
SSE/second-socket as a later upgrade noted here, not built now.

## Wire surface

DTO-first in `control_dto.cppm`: `AlarmEventDto` (`{ seq, fingerprint, metric, pass?, severity,
state: FIRING|RESOLVED, valueMs, thresholdMs, sinceFrame, count }`), `DrainAlarmsParams`
(`{ since }`) → `DrainAlarmsResult` (`{ events, highWaterSeq, overflowed, oldestSeq }`), and
`ActiveAlarmsDto`. Commands `drain-alarms` and `list-active-alarms`, plus `se` verbs (per the
control AGENTS.md "keep the `se` CLI usable" rule) so alarms are inspectable from a shell. Regenerate
with `bun run tools/gen-control-dto/gen.ts`.

## Files touched

| What | File | Symbols |
|---|---|---|
| Detectors (EMA, hysteresis, MAD, burn-rate) | `engine/source/saffron/rendering/renderer.cppm` (+ types) | per-frame `endFrame` tick reading the Phase 2 ring |
| Alarm state: ring, active set, seq, fingerprint | `engine/source/saffron/rendering/renderer_types.cppm` | `AlarmState` |
| DTOs + commands | `engine/source/saffron/control/control_dto.cppm`, `control_commands_render.cpp` | `AlarmEventDto`, `drain-alarms`, `list-active-alarms` |
| CLI | `tools/se` | `se drain-alarms`, `se alarms` |
| Docs | `docs/content/explanations/rendering/performance-alarms.md` (new) + hub row | — |

## Validation

- `make engine` + `make prepare-for-commit` clean.
- e2e: boot headless, `set-perf-config` to a tight budget (so normal frames breach), drive enough
  frames to cross the debounce, then `drain-alarms`; assert a FIRING `AlarmEventDto` appears with a
  monotonic `seq`; relax the budget and assert a matching **RESOLVED** event with the same
  fingerprint; verify a second `drain-alarms` with the returned `since` yields no duplicates.
- Docs page covering the smoothing/hysteresis/debounce math, the three detectors with their numbers,
  the severity table, and the non-blocking-drain + seq-cursor protocol.
