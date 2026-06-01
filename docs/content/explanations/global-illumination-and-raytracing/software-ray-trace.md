+++
title = 'Software ray trace'
weight = 4
math = true
+++

# Software ray trace

Each DDGI probe gathers its incoming light by tracing 64 rays into the scene, in software, through
the [voxel proxy](../voxel-scene-proxy/). No ray-tracing hardware is involved — the march is a
plain fixed-step loop in a compute shader, so the entire DDGI path works on the llvmpipe dev GPU.
`ddgi_trace.slang` is the pass.

The dispatch is one thread per `(probe, ray)` pair: a 64-wide thread group over rays, one group
per probe. Each thread picks its ray direction, marches the voxels, and writes radiance + hit
distance into a `rays × probeCount` image the blend passes read.

## Fibonacci-sphere ray directions

The 64 directions per probe are spread evenly over the sphere with a spherical Fibonacci sequence.
Direction $i$ of $n$ is

$$
\varphi = 2\pi \,\operatorname{frac}\!\big(i\,\phi^{-1} + r\big), \qquad
\cos\theta = 1 - \frac{2i + 1}{n}, \qquad
\sin\theta = \sqrt{1 - \cos^2\theta}
$$

with $\phi^{-1} = 0.618033\ldots$ the golden-ratio conjugate and $r$ a per-frame rotation offset.
The $\cos\theta$ term steps uniformly in height (equal-area latitude bands) and the golden angle
spirals the azimuth, so the points never clump.

The per-frame rotation $r = \operatorname{frac}(\text{frame} \cdot \phi^{-1})$ turns the fixed
64-ray set every frame, so the temporal blend averages many *different* directions over time — 64
rays per frame, effectively far more once converged.

## Marching the voxels

From the probe center, the ray steps in fixed increments of half the smallest voxel dimension, up
to 256 steps or the volume diagonal. At each step it converts the world position to a voxel
coordinate and reads occupancy; `voxel.a > 0.5` is a hit. It's a fixed-step march, not a true DDA:
simple and branch-light, at the cost of possibly stepping over a thin voxel or sampling the same
voxel twice. For a 32³ proxy whose features are already coarse, that's an acceptable trade.

## Radiance on a hit, sky on a miss

On a hit the radiance is the voxel albedo lit by a crude direct term (sky ambient plus a
half-strength sun) plus a multi-bounce contribution; on a miss the ray escaped and returns the sky
color:

$$
L_\text{hit} = \rho \,(L_\text{sky} + \tfrac12\, L_\text{sun}\, I_\text{sun}) \;+\; \tfrac14\,\rho \, E_\text{prev}
$$

## Multi-bounce by reading last frame

The $E_\text{prev}$ term is what makes the bounce light multiply. At a hit, the shader samples
*last frame's* irradiance atlas in the ray direction and folds a quarter of it (times the hit
albedo) back into this ray's radiance. Because that atlas was itself fed by the previous frame's
bounce, each frame adds one indirect bounce and the temporal blend settles to many bounces over a
fraction of a second — no extra rays, just the feedback loop.

The probe whose atlas it samples is the *same* probe doing the trace
(`sampleProbeIrradiance(p, dir)`), a cheap approximation of "gather the bounce at the hit point"
that works because the volume is low-frequency.

Each thread writes `float4(radiance, hitDist)` to `rayOut[ray, probeIndex]`. The radiance feeds
[the irradiance atlas](../irradiance-and-moment-atlases/) and the hit distance feeds the moment
atlas for Chebyshev visibility.

## In the code

| What | File | Symbols |
|---|---|---|
| The trace | `ddgi_trace.slang` | `computeMain` |
| Ray directions | `ddgi_trace.slang` | `sphericalFibonacci` |
| Multi-bounce sample | `ddgi_trace.slang` | `sampleProbeIrradiance` |
| Probe world position | `ddgi_trace.slang` | `probeWorldPos` |
| Trace graph pass | `renderer.cppm` | `ddgi-trace` pass (the `doDdgi` block) |

> [!NOTE]
> The bounce term reads the same probe's *previous* irradiance, not the irradiance at the actual
> hit point's nearest probe. It biases the result slightly but avoids a second voxel→probe lookup
> per ray. The volume's low spatial frequency hides the error.

## Related

- [Voxel proxy](../voxel-scene-proxy/) — what the rays march through
- [Probe atlases](../irradiance-and-moment-atlases/) — where the radiance and distance go
- [Acceleration structures](../raytracing-foundation/) — the BLAS/TLAS path this trace stands in for
