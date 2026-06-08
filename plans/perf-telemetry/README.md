# perf telemetry plan

This plan turns the engine's thin render-stats snapshot into a real performance-telemetry
pipeline: a CPU-vs-GPU frame breakdown with per-pass GPU timing, a rolling frame-time history
with percentile/stutter quality metrics, renderer-emitted **alarms** pushed to the editor when
performance degrades, and a live visualization in the Tauri editor — all driven over the existing
control plane and scriptable from the `se` CLI.

## Why

Today the only performance signal is `RenderStats` (`renderer_types.cppm:552-560`): CPU
`frameMs` (EMA-smoothed `steady_clock` delta at `renderer.cppm:2077-2082`), derived `fps`, draw
counters (`renderer_drawlist.cpp:446-448`), and a **`gpuMs` stubbed at `0.0`**
(`renderer_types.cppm:559`). It is exposed as a single instantaneous snapshot through the
`render-stats` command (`control_commands_render.cpp:127-130`), polled at ~6 Hz by the editor's
reconcile loop (`editor/src/state/store.ts`, `FAST_RECONCILE_INTERVAL_MS = 50`) and shown as flat
numbers in `RenderStatsPanel.tsx`. There is no GPU timing, no per-pass attribution, no history, no
notion of "good vs bad", and no way to be *told* when something degrades — you have to be staring
at the panel.

The gap, concretely:

- **No CPU/GPU split.** A single `frameMs` cannot say whether a slow frame is CPU-bound (GPU idle,
  waiting for work) or GPU-bound (CPU blocked on a fence/present). `gpuMs` is a stub.
- **No per-pass attribution.** The render graph records named passes (`RgPass.name`,
  `render_graph.cppm`) but times none of them, so "which pass regressed" is unanswerable in-engine.
- **No time dimension.** `render-stats` is a snapshot; the editor keeps no buffer, so stutter
  (which lives in the *distribution* of frame times, not the average) is invisible.
- **No push.** The control plane is strictly request/response, pumped synchronously once per frame
  on the main thread (`pollControl`, `host.cppm:525-529`); there is no channel to notify the editor
  the instant a frame budget is blown.

## What "done" looks like

- Per-frame **CPU time, GPU time, and per-pass GPU ms** measured with Vulkan timestamp queries,
  plus CPU throughput counters and VMA memory budget — gated behind a profiler mode so the
  present-only host stays cheap.
- A rolling **frame-time history** with p50/p95/p99/p99.9 lows and a stutter detector, against a
  **single shared budget/threshold config** so the engine, the editor HUD, and e2e tests all agree
  on green/amber/red.
- Renderer **alarms** (EMA-smoothed, hysteresis + debounce, MAD-spike and burn-rate detectors)
  delivered over a **non-blocking `drain-alarms`** command with a sequence cursor — no render-loop
  stall, no missed or duplicated events.
- An editor **metrics dashboard**: a live uPlot frame-time graph, per-pass bars, a VRAM gauge, and
  alarm toasts + an alarm log + an active-alarms badge.
- The whole thing scriptable from `se` and covered by an e2e perf-smoke test.

## A caveat that shapes the whole plan: software GPU

The toolbox commonly runs on **Mesa llvmpipe / lavapipe — a software rasterizer where "the GPU"
is the CPU**. There, GPU timestamps, saturation, and occupancy are CPU rasterization time and are
**not representative of real hardware**. Every phase that emits GPU numbers must detect the device
name at init and **tag/annotate software-GPU runs** so nobody draws hardware conclusions (and so
the e2e gate does not assert hardware-shaped numbers under llvmpipe). In-engine queries answer
*what* is slow; the micro-architectural *why* (occupancy, VALU util, cache/DRAM-bound) still needs
a vendor profiler (Radeon GPU Profiler / Nsight) and is explicitly out of scope.

## Status convention

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`). Mark a
phase `COMPLETED` when its work is done and validation-clean (`make engine` + `make
prepare-for-commit`, and the editor phase also `bun run check` + `bun run build`); delete a phase
file only *after* it is `COMPLETED` and merged. Per the root `AGENTS.md` "keep current" rule, each
phase ends by updating its `docs/content/` concept page and adding/extending its test in the same
change — docs and tests are not deferred to the end.

## Phases

| Phase | What | Status |
|---|---|---|
| [1 — engine instrumentation](phase-1-engine-instrumentation.md) | CPU/GPU split, per-pass GPU timestamp queries in the render graph, throughput counters, VMA budget; profiler-mode gate; llvmpipe tagging; extended `RenderStatsDto` | COMPLETED |
| [2 — frame history & quality metrics](phase-2-frame-history-and-quality.md) | engine-side frame-time ring buffer, p50/p95/p99/p99.9 lows, stutter detector, shared budget/threshold config, `frame-history` command | COMPLETED |
| [3 — alarms & event delivery](phase-3-alarms-and-delivery.md) | EMA + hysteresis + debounce, MAD-spike & burn-rate detectors, severity, non-blocking `drain-alarms` with seq cursor, fingerprint dedup + resolve | COMPLETED |
| [4 — editor visualization](phase-4-editor-visualization.md) | Zustand time-series slice, uPlot live graph, per-pass + VRAM views, shared color thresholds, static shadcn/Recharts panels, alarm toasts/log/badge | COMPLETED |

## Sequencing

Strictly dependency-ordered. Phase 1 is the data source everything else reads; it is independently
valuable on its own (it makes `gpuMs` real and the existing panel immediately better). Phase 2 adds
the time dimension and the good/bad definition. Phase 3 needs 1–2's signals to alarm on. Phase 4
consumes 1–3 over the wire. A reasonable first shippable slice is **Phase 1 + the live uPlot graph
slice of Phase 4** if you want a visible win before building the history/alarm layers.
