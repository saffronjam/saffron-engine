# Phase 2 — frame history & quality metrics

**Status:** COMPLETED

> Engine-side `FrameHistoryCapacity`-deep ring (raw per-frame `{cpuMs, gpuMs, cpuWaitMs}`,
> pushed every frame at `endFrame`, always on); on-demand p50/p95/p99/p99.9 + max/stddev/mean
> percentiles; the relative-2×-plus-floor stutter detector; the shared `PerfConfig`
> (`targetFps`-derived budget + green/amber/red thresholds) over `frame-history` /
> `get-perf-config` / `set-perf-config`, with `se` verbs and an e2e test. Frame time is
> `cpuMs + cpuWaitMs` (render-thread wall clock). Per-pass *soft* budgets (a pass's allowed
> %-of-frame) are deferred to the Phase-4 coloring work; the rest of the config table is in.

Add the time dimension and the definition of "good vs bad". Phase 1 makes per-frame CPU/GPU numbers
real but still one-shot; this phase keeps a rolling history in the engine, computes the
percentile/stutter metrics that actually correlate with perceived smoothness, and pins down a
**single shared budget/threshold config** that the engine, the editor HUD, and e2e tests all read,
so green/amber/red means the same thing everywhere.

## Why engine-side history (not just client-side)

The editor polls at ~6 Hz (`store.ts`, `FAST_RECONCILE_INTERVAL_MS = 50`), but the engine renders
at the display rate. Percentiles and stutter live in the *per-frame* distribution, so they must be
computed from every frame, not from 6 Hz samples — that means the engine owns the ring buffer.
Recording the raw per-frame totals from Phase 1 (not the EMA-smoothed display value) keeps the
distribution honest; smoothing is for the display/alarm path only.

## Frame-time ring buffer

A fixed-size ring (e.g. last `N = 1024` frames ≈ 8–17 s at 60–120 Hz) of compact samples in the
renderer: `{ cpuMs, gpuMs, cpuWaitMs }` per frame, pushed once per frame at `endFrame`. O(1) memory,
no per-frame allocation. This is the source for both the percentile summary (this phase) and the
alarm detectors (Phase 3), so site it where both can read it (`Renderer`, `renderer_types.cppm`).

## Quality metrics

Compute over the ring on demand (when queried) — not every frame:

- **Percentiles / lows.** p50 (median, the primary number — outlier-resistant), p95, p99, p99.9
  frame times = the 5% / 1% / 0.1% lows (the p99 *frame time* is the 1%-low *FPS*). Average FPS is
  deliberately demoted; it oversamples fast frames and hides hitches.
- **Consistency.** The p50→p99 gap (small = smooth) plus stddev and max frame time.
- **Stutter detector.** Flag a frame as a stutter when `frametime > 2 × avg(previous 3 frames)`
  **AND** over an absolute floor (≈ `2 × budget`); keep a per-session stutter count and the last
  stutter timestamp. This relative-2×-plus-floor rule is what catches hitches independent of the
  absolute frame rate.

## The shared budget/threshold config

One config object is the single source of truth, surfaced over the wire so the editor and tests
read identical numbers (do not hardcode thresholds in the editor):

| Knob | Default | Meaning |
|---|---|---|
| `targetFps` | 60 | budget = `1000 / targetFps` (16.67 ms); selectable 30/60/90/120 + custom |
| budget cliff | `1.0 × budget` | over-budget = a dropped frame (hard cliff, not a slope) |
| green | `< 0.8 × budget` **and** `< 1.5 × median` | within budget with headroom, consistent |
| amber | `≥ 0.8 × budget`, or `1.5–2 × median` | near budget / occasional spike |
| red | `> budget`, or `> 2 × median`, or frozen band | over budget / true stutter |
| frozen band | `> ~250 ms` | a hard hitch → always red |
| VRAM warn / crit | 80–95% / ≥100% of budget | from Phase 1's `usage/budget` |

A VR profile swaps the budget to 11.11 ms (90 Hz) and tightens amber (presence breaks before
smoothness does). Expose `get-perf-config` / `set-perf-config` commands; the editor's target-refresh
dropdown drives every threshold and the HUD color scale from this one place. Per-pass soft budgets
(a pass's allowed %-of-frame) live here too, so a pass colors amber/red against its own share.

## Wire surface

DTO-first in `control_dto.cppm`:

- `FrameHistoryDto` — `{ p50Ms, p95Ms, p99Ms, p999Ms, maxMs, stddevMs, stutterCount, samples? }`,
  where `samples` (the recent raw ring, optional/bounded) lets the editor draw the live graph
  without keeping its own high-rate buffer. Returned by a new `frame-history` command.
- `PerfConfigDto` — the table above; `get-perf-config` / `set-perf-config`.

Add matching `se` verbs (`se frame-history`, `se get-perf-config`) so smoothness and budgets are
inspectable from a shell. Regenerate with `bun run tools/gen-control-dto/gen.ts`.

## Files touched

| What | File | Symbols |
|---|---|---|
| Ring buffer + percentile/stutter compute | `engine/source/saffron/rendering/renderer_types.cppm`, `renderer.cppm` | `Renderer`, `endFrame` |
| Perf config (shared thresholds) | `engine/source/saffron/rendering/renderer_types.cppm` | `PerfConfig` |
| DTOs + commands | `engine/source/saffron/control/control_dto.cppm`, `control_commands_render.cpp` | `FrameHistoryDto`, `PerfConfigDto`, `frame-history`, `get/set-perf-config` |
| CLI | `tools/se` | new verbs |
| Docs | `docs/content/explanations/rendering/performance-telemetry.md` | extend with percentiles + thresholds |

## Validation

- `make engine` + `make prepare-for-commit` clean.
- e2e: boot headless, run frames, call `frame-history`; assert the percentile fields are present and
  ordered (`p50 ≤ p95 ≤ p99 ≤ p999 ≤ max`) and `stutterCount` is an integer; round-trip
  `set-perf-config { targetFps: 30 }` and confirm the budget-derived numbers change.
- Docs page extended with the budget table, the percentile rationale (why not average FPS), and the
  stutter rule.
