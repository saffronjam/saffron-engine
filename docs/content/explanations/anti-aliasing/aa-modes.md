+++
title = 'AA modes'
weight = 3
+++

# AA modes

The renderer offers three anti-aliasing approaches. They are mutually exclusive, and one call
configures all of them: `setAa(renderer, msaaSamples, fxaa, taa)`. The `se set-aa` command is
the CLI front for it.

## The modes

| Mode | `set-aa` arg | What it is | Where |
|---|---|---|---|
| Off | `off` | no anti-aliasing | — |
| MSAA 2× / 4× / 8× | `msaa2` / `msaa4` / `msaa8` | multisampled scene color + depth, resolved into the offscreen | [MSAA](../msaa/) |
| FXAA | `fxaa` | luma-edge blur, one compute pass on a 1× scratch → offscreen | [FXAA](../fxaa/) |
| TAA | `taa` | history reprojection + neighbourhood clamp + exponential blend | [TAA](../../screen-space-and-post/taa/) |

## Selecting a mode

`setAa` takes a sample count plus FXAA and TAA flags and resolves them into one active mode.
MSAA wins if a sample count above 1 is requested; otherwise FXAA, then TAA. The count maps to
a `vk::SampleCountFlagBits` and clamps to the device's `maxSampleCount`, so asking for `msaa8`
on hardware that tops out at 4× quietly gives you 4×.

The `set-aa` command parses the mode string into those three arguments:

| String | `msaaSamples` | `fxaa` | `taa` |
|---|---|---|---|
| `off` | 1 | false | false |
| `msaa2` / `msaa4` / `msaa8` | 2 / 4 / 8 | false | false |
| `fxaa` | 1 | true | false |
| `taa` | 1 | false | true |

## What a switch does

A switch is a full reconfigure, not a flag flip. `setAa` waits for the GPU to go idle (the
targets and PSOs are about to be destroyed), stores the new sample count and flags, then
recreates three target sets: the multisampled MSAA pair, the 1× scratch that FXAA and TAA
share, and the TAA motion + history pair. Finally it clears the PSO cache and rebuilds the
depth-prepass pipeline, because the mesh and prepass PSOs bake the sample count.

## Reading back the active mode

`aaMode(renderer)` reports the current mode as a string, and `se render-stats` exposes it as
the `aa` field. FXAA and TAA report by their flag; otherwise the sample count decides — `"off"`
at 1, `"msaaN"` above.

## In the code

| What | File | Symbols |
|---|---|---|
| Configure + clamp + rebuild | `renderer_aa.cpp` | `setAa`, `aaMode` |
| CLI front | `control_commands_render.cpp` | `set-aa`, `render-stats` · `aa` |
| Mode flags | `renderer_types.cppm` | `sampleCount`, `maxSampleCount`, `fxaaEnabled`, `taaEnabled` |

## Related

- [MSAA](../msaa/) — the rasterization-time mode
- [FXAA](../fxaa/) — the cheap post-process mode
- [TAA](../../screen-space-and-post/taa/) — the temporal mode
