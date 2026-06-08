# Phase 1 — engine instrumentation

**Status:** COMPLETED

> Per-pass GPU timestamps, the CPU/GPU split, throughput counters, the VMA budget read,
> software-GPU tagging, and the `profiler.set-mode` / `pass-timings` commands are all in.
> `pipeline-stats` is a wire-stable, capability-gated mode whose per-pass *statistics decode*
> (overdraw / clipping / shader-invocation counts) is deferred to a focused follow-up — the
> mode is accepted and reported, and currently captures the same timestamp data as
> `timestamps`. Everything in the validation gate below passes; the deferred decode is not
> part of that gate.

Make the raw numbers real. Today the only honest performance metric is CPU `frameMs`; `gpuMs` is a
literal `0.0` stub and nothing attributes cost to a pass. This phase measures CPU time and GPU time
*separately*, times every render-graph pass on the GPU, accumulates the missing CPU throughput
counters, and reads VMA's memory budget — all behind a profiler mode so the present-only host pays
nothing by default. The deliverable is an extended `RenderStatsDto` whose new fields the existing
`RenderStatsPanel` immediately benefits from (no more zero `gpuMs`).

## The CPU/GPU split is the headline

The CPU records frame N+1 while the GPU executes frame N, so the two are independent timelines. A
single frame-time number cannot tell CPU-bound (GPU idle, waiting for work) from GPU-bound (CPU
blocked on a fence/present). Measure both:

- **CPU phase times** with `steady_clock` around the existing seams: `onUpdate`, the submit-lambda
  replay / command recording, `beginFrameGraph`, and `executeRenderGraph`
  (`renderer.cppm:1948`). The current single timestamp at `endFrame`
  (`renderer.cppm:2077-2082`, `lastFrameNs` at `renderer_types.cppm:1204-1205`) becomes the
  total-CPU number; add the phase breakdown alongside it.
- **CPU blocking-wait time** separately: time spent in `waitForFences` at `beginFrame`
  (`renderer.cppm:701`) and at present. High wait time ⇒ GPU-bound; GPU idle between submits ⇒
  CPU-bound. This single number is what lets the editor label each frame's bottleneck.

Keep the EMA smoothing pattern already used for `frameMs` for the headline numbers, but also feed
the *raw* per-frame CPU and GPU totals into Phase 2's history ring (smoothing belongs on the
display/alarm path, not on the recorded series).

## Per-pass GPU timing via timestamp queries

The render graph is the natural home: `executeRenderGraph` (`render_graph.cppm:284`) already walks
`graph.passes` in order, derives barriers, and brackets each `RgPass.execute` body. Wrap each pass
body in a pair of timestamp writes — the graph already owns the seam, so no pass writes a query by
hand (same principle as "no pass writes a barrier by hand").

Mechanics (all standard Vulkan timestamp-query rules):

- Allocate a `vk::QueryPool` of type `eTimestamp` **per frame-in-flight** (the renderer already
  rings `MaxFramesInFlight`; `frame.index` advances at `renderer.cppm:2084`). Size = `2 *
  maxPasses` slots.
- `cmd.resetQueryPool(...)` for this frame's pool at the start of recording (queries are
  uninitialized and must be reset before use; reading an unreset pool can cause device loss).
- In `executeRenderGraph`, `cmd.writeTimestamp2(eTopOfPipe, pool, base)` before the body and
  `writeTimestamp2(eBottomOfPipe/eAllCommands, pool, base+1)` after, recording the pass name →
  slot mapping.
- Read back the **previous** frame's pool — when `beginFrame` waits on `frame.inFlight`
  (`renderer.cppm:701`) that slot's GPU work (from `MaxFramesInFlight` frames ago) is complete, so
  `getQueryPoolResults(... e64 | eWithAvailability)` is non-blocking. **Never** use `eWaitBit` in
  the loop.
- Convert ticks → ns with `VkPhysicalDeviceLimits::timestampPeriod`; **mask** each raw value to the
  low `timestampValidBits` of the graphics queue family before subtracting.

Result: a `std::vector<{name, gpuMs}>` per frame, keyed by the graph's pass names, plus a summed
GPU total. Caveats to honor in the data model and the UI copy: adjacent/async passes overlap so
per-pass numbers are relative (the total is not a clean sum of parts), and an underclocked/idle GPU
inflates timings (cross-check against CPU wall-clock — Phase 2/3 use this).

## Pipeline statistics (opt-in within the profiler mode)

Behind the deepest profiler level only, add `ePipelineStatistics` query pools per pass to answer
*why* a pass is slow: `FRAGMENT_SHADER_INVOCATIONS` vs rendered pixels = overdraw;
`CLIPPING_INVOCATIONS` vs `CLIPPING_PRIMITIVES` = is culling working; `IA_VERTICES` vs
`VS_INVOCATIONS` = vertex reuse; `COMPUTE_SHADER_INVOCATIONS` for the GI/lighting compute passes.
Requires the `pipelineStatisticsQuery` device feature — check it at device creation and degrade
gracefully if absent. This is the heaviest capture; keep it strictly opt-in.

## CPU throughput counters

Accumulate per frame at the submit/record seam (extend the existing draw counters at
`renderer_drawlist.cpp:446-448`), reset each frame: draw calls, instances, **triangles**
(sum `Submesh.indexCount/3`), dispatches, command-buffer count, `queueSubmit` count, descriptor
binds, descriptor updates, and **pipelines-created-this-frame** (non-zero on a steady-state frame
is the signature of a PSO-compile hitch — wired to a Phase 3 alarm). These explain CPU-bound frames
and confirm the bindless + submesh-major instancing path keeps binds ~constant, not O(draws).

## VMA memory budget

Enable `VK_EXT_memory_budget` at device creation, then call `vmaGetHeapBudgets` each frame (cheap;
unlike `vmaCalculateStatistics`, which stays behind an explicit deep-stats request). Expose per-heap
`usage`, `budget`, the `usage/budget` ratio, `allocationCount`, and `blockBytes − allocationBytes`
(fragmentation slack). `usage` approaching `budget` predicts eviction thrashing — the input to a
Phase 3 VRAM alarm.

## Profiler-mode gate

None of the above runs by default. Add a `profiler.set-mode {off|timestamps|pipeline-stats}`
control command (one typed `registerCommand` per `command.cppm`) plus a matching `se` verb. `off` is
the default and keeps the present-only host at today's cost; `timestamps` enables per-pass GPU timing
+ counters + VMA; `pipeline-stats` adds the statistics queries. Verify `timestampValidBits != 0`
and `timestampComputeAndGraphics` at init and report capability so the editor can disable controls
the device can't support.

## Software-GPU tagging

Detect `llvmpipe`/`lavapipe` from the physical device name at init and set a `softwareGpu` flag on
the stats payload. Downstream (editor + e2e) annotates or suppresses GPU-timing numbers when set —
they are CPU rasterization time, not GPU time.

## Wire surface

Extend the DTOs in `control_dto.cppm` (DTO-first; the schema dir is generated, per
`schemas/control/AGENTS.md`). The existing `RenderStatsDto` (`control_dto.cppm:148-171`) grows:
`cpuFrameMs`, `gpuFrameMs` (replacing the `gpuMs` stub semantics), `cpuWaitMs`, the throughput
counters, `vramUsageBytes`/`vramBudgetBytes`, `softwareGpu`, and `profilerMode`. Add a sibling
`RenderPassTimingsDto` (`{ passes: [{name, gpuMs, ...stats}], gpuTotalMs }`) returned by a new
`pass-timings` command, so the heavy per-pass array is not on the hot `render-stats` path. Keep DTO
field order = positional CLI order. Run `bun run tools/gen-control-dto/gen.ts` and commit the
regenerated serde, TypeScript, OpenRPC, and manifest.

## Files touched

| What | File | Symbols |
|---|---|---|
| Stats struct + timing fields | `engine/source/saffron/rendering/renderer_types.cppm` | `RenderStats`, `Frame` |
| Per-pass timestamp pools + read-back | `engine/source/saffron/rendering/render_graph.cppm` | `RenderGraph`, `executeRenderGraph` |
| CPU phase/wait timing, pool reset/rotate | `engine/source/saffron/rendering/renderer.cppm` | `beginFrame`, `endFrame` |
| Throughput counters | `engine/source/saffron/rendering/renderer_drawlist.cpp` | `submitDrawList` |
| VMA budget read | `engine/source/saffron/rendering/renderer.cppm` (+ device-create) | memory-budget extension enable |
| DTOs + commands | `engine/source/saffron/control/control_dto.cppm`, `control_commands_render.cpp` | `RenderStatsDto`, `RenderPassTimingsDto`, `profiler.set-mode`, `pass-timings` |
| CLI | `tools/se` | new verbs |
| Concept page | `docs/content/explanations/rendering/performance-telemetry.md` (new) + hub `_index.md` row | — |

## Validation

- `make engine` + `make prepare-for-commit` clean.
- Headless run with `profiler.set-mode timestamps`: `pass-timings` returns positive `gpuMs` for
  the scene pass and a per-pass list whose names match the graph; under llvmpipe the `softwareGpu`
  flag is set.
- e2e perf-smoke (`tests/e2e`, bun over the control plane): boot headless, set profiler mode, run N
  frames, assert `gpuFrameMs > 0` (skip/relax the magnitude assertion when `softwareGpu`), the
  per-pass breakdown is non-empty, and throughput counters are non-zero. This keeps instrumentation
  honest as the graph changes.
- New `docs/content` page covering the CPU/GPU split, the timestamp mechanism with the
  `timestampPeriod`/`timestampValidBits`/overlap caveats, the counters, and the VMA budget loop,
  with a slim `What | File | Symbols` table.
