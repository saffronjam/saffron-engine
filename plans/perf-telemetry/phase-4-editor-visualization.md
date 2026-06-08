# Phase 4 — editor visualization

**Status:** COMPLETED

> A second ~10 Hz metrics polling lane in the reconcile loop (alarms always, for the badge;
> frame-history + pass-timings only while the Stats panel is open) fills new Zustand
> telemetry slices; client wrappers + the `lib/perfThresholds.ts` shared green/amber/red
> module back it. The dashboard (in `RenderStatsPanel`) has a uPlot live frame-time graph
> with a budget line, a budget-fill bar + CPU/GPU-bound call + p50/p95/p99/p99.9 lows, a
> per-pass GPU breakdown (ms + %-of-budget), a VRAM gauge, a Profiler switch + Target-FPS
> dropdown, and a software-GPU banner. Alarms route by severity to throttled/persistent
> `sonner` toasts + an alarm log + the topbar active-alarms badge. `bun run check` / `lint`
> / `build` and the Hugo docs build are clean.
>
> **Deferred:** per-pass *soft* budgets (a pass coloured against its own %-share, mirroring
> the Phase-2 config knob that was itself deferred) — the per-pass bars currently grade
> against a single share of the frame budget. **Not done here:** the interactive visual
> check (`make run`) needs a Wayland session + the engine subsurface, so it was not run in
> the headless toolbox; the build/check/lint/docs gate stands in for it.

Make it visible and helpful. Phases 1–3 expose CPU/GPU timing, per-pass breakdowns, percentile
quality metrics, shared thresholds, and alarm events over the wire. This phase consumes them in the
Tauri editor: a live frame-time graph, per-pass bars, a VRAM gauge, and the alarm UX (toasts + log +
badge) — colored from the one shared `PerfConfig` so the HUD agrees with the engine and the tests.

## Charting: shadcn covers static, uPlot covers live

shadcn *does* ship a Chart component, but it is a copy-paste wrapper over **Recharts (SVG)** — every
point is a DOM node and every tick a React re-render, which does not scale to a 30–120 Hz frame-time
graph. Split by frequency:

- **Static / low-frequency panels** (per-pass bar breakdown, a one-shot percentile histogram, KPI
  cards): use the shadcn Chart (`bunx shadcn@latest add chart`, Recharts v3 — React 19 native, no
  `--legacy-peer-deps`). Themed from the existing `--chart-*` CSS variables; we own the source.
- **The live frame-time / GPU graph**: use **uPlot** (~48 KB, Canvas 2D, dependency-free; ~10% CPU
  streaming thousands of points at 60 fps). This matters because the webview composites *over* the
  live engine viewport — editor CPU is not free.

uPlot integration rules: create the instance once in a `useEffect` (ResizeObserver for sizing); keep
samples in a **fixed typed-array ring buffer**, not React state; call `chart.setData()` on a
`requestAnimationFrame` tick fed by the poll. Theme it by reading the same shadcn tokens via
`getComputedStyle(document.documentElement).getPropertyValue('--chart-1')` (and `--border`,
`--muted-foreground`), re-reading on dark-mode change so the canvas matches the Recharts panels.
Sparklines (a frame-time pulse in the status bar) reuse the uPlot wrapper in an axis-less config or a
hand-rolled `<canvas>` — don't pull Recharts in for a mini-widget.

Add `uplot` to `editor/package.json`; confirm peers under bun.

## Data flow: a time-series store slice

Today `renderStats` is a single replaced snapshot in the Zustand store (`store.ts`) and the reconcile
loop polls `getSelection` + `renderStats` + `getGizmo` every 50 ms (`startReconcile`,
`FAST_RECONCILE_INTERVAL_MS = 50`). Extend, don't replace:

- A **time-series slice**: ring buffers (typed arrays) for cpu/gpu/frame ms, fed either by the
  per-frame `samples` returned in Phase 2's `FrameHistoryDto` (preferred — gives true per-frame
  resolution despite the 6 Hz poll) or by the snapshot when samples are absent.
- Add `frame-history`, `pass-timings`, and `drain-alarms` to the reconcile `Promise.all` (gated to
  when the metrics dashboard is open, to keep idle cost low). Reuse the `coalesce.ts` pattern if any
  control writes (e.g. `set-perf-config` from the target-FPS dropdown) need throttling.
- The new client wrappers go in `editor/src/control/client.ts` against the regenerated
  `@saffron/protocol` types (`bun run check` / `gen:protocol`).

## The metrics dashboard

A dedicated view (promote beyond the current bottom-dock `RenderStatsPanel.tsx` tab — a larger panel
or drawer):

- **Live frame-time graph** (uPlot): CPU, GPU, and total ms as series, with a horizontal **budget
  line** and a thin frozen-band line from `PerfConfig`; rolling window ~600–1200 samples (~10–20 s).
- **Headline numbers**: p50 primary, with p95/p99/p99.9 labeled as the 5%/1%/0.1% lows; a
  CPU-bound-vs-GPU-bound indicator from `cpuWaitMs`/`gpuFrameMs`; a budget-fill bar (used% =
  frametime/budget) for headroom at a glance. Demote average FPS.
- **Per-pass breakdown**: each pass in **ms and %-of-budget** (e.g. "shadows 0.42 ms / 2.5%") with a
  stacked "GPU total vs budget" bar; color each pass against its per-pass soft budget. A tooltip
  notes the overlap caveat (per-pass numbers are relative; the total isn't a clean sum).
- **VRAM gauge**: `usage/budget` colored green < 80% / amber 80–95% / red ≥ 100%.
- **Target-refresh dropdown** (30/60/90/120 + custom + VR) → `set-perf-config`, driving every
  threshold and the color scale from one source of truth.
- **Software-GPU banner**: when `softwareGpu` is set, badge the GPU numbers as "software rasterizer
  (llvmpipe) — not representative" so nobody reads hardware conclusions.

## Color thresholds: one shared module

Mirror `PerfConfig` in a single TS module that returns green/amber/red for a value, used by the
graph, the numbers, the per-pass bars, and the gauge — never hardcode thresholds per component.
Scheme (from the engine config): 🟢 `< 0.8 × budget` and `< 1.5 × median`; 🟡 within budget but
`> 0.8 × budget`, or `1.5–2 × median`; 🔴 `> budget`, or `> 2 × median`, or frozen band.

## Alarm UX (severity-routed, fatigue-aware)

Consume Phase 3's `drain-alarms` (with the `since` cursor persisted in the store) and
`list-active-alarms`:

- **info** → alarm log only (no interruption).
- **warning** → a **throttled** toast (reuse the installed `sonner`; repeat interval ≥ 10 s per
  fingerprint) **plus** highlight the offending pass row red/amber in the per-pass breakdown.
- **critical** → a persistent alarm-log entry (stays until acknowledged) + the **active-alarms
  badge**.
- Coalesce by fingerprint (a flapping pass = one entry, not 30); auto-clear the toast/highlight on
  the RESOLVED event. An **active-alarms badge** (red if any critical, amber if any warning) bound to
  `list-active-alarms` gives ambient awareness; clicking it opens the log.

## Files touched

| What | File |
|---|---|
| Time-series store slice + cursor + alarm state | `editor/src/state/store.ts` |
| Client wrappers | `editor/src/control/client.ts` |
| uPlot wrapper + shared threshold module | `editor/src/components/` (new), `editor/src/lib/` (new) |
| Metrics dashboard + per-pass + VRAM + dropdown | `editor/src/panels/RenderStatsPanel.tsx` (expand) or a new panel |
| Alarm toasts/log/badge | new components + topbar badge (`editor/src/panels/Topbar.tsx`) |
| Deps | `editor/package.json` (`uplot`, shadcn `chart`) |
| Docs | `docs/content/explanations/ui-and-editor/` (editor metrics dashboard) |

## Validation

- `cd editor && bun run check && bun run lint && bun run build` clean (regenerated protocol types).
- `make editor` / `make check` green; visual check via `make run`: live graph tracks the budget line,
  per-pass bars sum sensibly, target-FPS dropdown recolors everything, an induced over-budget
  condition raises a toast that auto-clears on recovery, and the software-GPU banner shows under
  llvmpipe.
- Docs page for the editor metrics dashboard with a `What | File | Symbols` table.
