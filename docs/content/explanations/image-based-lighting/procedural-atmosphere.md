+++
title = 'Procedural atmosphere'
weight = 4
math = true
+++

# Procedural atmosphere

The procedural atmosphere is a physically based sky (Hillaire 2020) that fills the same environment cube the [procedural sky](../procedural-sky/) gradient otherwise paints. When enabled it replaces the analytic gradient with the in-scattered radiance of a Rayleigh + Mie + ozone atmosphere lit by the scene's directional light, so the visible sky, the IBL convolutions, and the directional-light-driven re-bake all re-tint coherently from one source.

It is a *source switch*, not a new pipeline. The cube it produces is opaque to everything downstream: the irradiance and prefilter passes, the BRDF lookup, and the visible-sky pass consume it exactly as they consume the gradient. The only new work is a short chain of lookup tables baked just before the cube fill.

## The LUT chain

The bake evaluates three lookup tables in order, each a compute dispatch reading the previous, then fills the cube from the last. All are small, persistent `rgba16f` images, allocated once and re-baked in place when the sun or atmosphere parameters change.

**Transmittance LUT** ($256 \times 64$). For each texel it maps $(u, v)$ to a view-zenith cosine and an altitude,

$$\mu = 2u - 1, \qquad r = r_\text{planet} + v \, h_\text{atm},$$

ray-marches to the top of the atmosphere, and stores $e^{-\tau}$ where $\tau$ is the accumulated Rayleigh + Mie + ozone optical depth along that ray. This is the fraction of light that survives a path to the atmosphere boundary — sampled later as "how much sunlight reaches this point."

**Multiple-scattering LUT** ($32 \times 32$). Indexed by $(\cos\theta_\text{sun}, \text{altitude})$, it integrates second-order in-scattering over a coarse sphere of directions and closes Hillaire's energy-conserving geometric series,

$$\Psi_\text{ms} = \frac{L_{2}}{1 - f_\text{ms}},$$

where $L_2$ is the doubly scattered radiance and $f_\text{ms}$ the fraction of light re-scattered per bounce. A fixed $8 \times 8$ sphere is enough for a correct result on software rasterization; raising the sample count is a precision increment, not a correctness one.

**Sky-view LUT** ($192 \times 108$). For each $(\text{azimuth}, \text{elevation})$ it ray-marches the in-scattered radiance from the camera altitude, applying the Rayleigh phase, the Henyey–Greenstein Mie phase

$$p_\text{HG}(\cos\theta) = \frac{1 - g^2}{4\pi \, (1 + g^2 - 2g\cos\theta)^{3/2}},$$

the transmittance toward the sun, and the multiple-scattering term. Elevation is horizon-densified with a $\sqrt{\cdot}$ mapping about the horizon so the bright, high-gradient band near $y = 0$ gets the most resolution.

## Filling the cube

`atmos_skygen` keeps the gradient shader's output contract: one invocation per cube texel, `tid.z` the face, writing into the 6-layer `rgba16f` cube. For each direction it inverts the sky-view mapping,

$$u = \frac{\text{azimuth}}{2\pi}, \qquad v = \tfrac12 + \operatorname{sign}(\text{el}) \, \sqrt{\tfrac{|\text{el}|}{\pi/2}} \cdot \tfrac12,$$

samples the sky-view LUT, and adds a sun disk — a `smoothstep` cap around the sun direction within `sunDiskAngularRadius`, scaled by `sunDiskIntensity`. The result is the cube the rest of IBL convolves.

## When it bakes

The atmosphere rides the existing on-demand re-bake. `renderScene` mirrors `scene.environment.atmosphere` onto the renderer-side params and selects the source by precedence — a user equirect panorama wins, then the atmosphere when `enabled`, then the gradient. `requestEnvBake` flags a re-bake only when the source, the sun, or any atmosphere field actually changes (an exact compare, so an unchanged frame never re-bakes), and `beginFrameGraph` consumes that flag at a GPU-idle frame start. Moving the sun therefore re-tints the visible sky and the IBL together, and toggling `enabled` off restores the gradient bit-for-bit.

The whole chain lives inside `bakeEnvironment` — a one-shot transient command buffer with manual barriers, like the irradiance/prefilter tail — not the per-frame render graph, because it only changes on a sun or parameter change.

## In the code

| What | File | Symbols |
|---|---|---|
| Transmittance | `atmos_transmittance.slang` | `computeMain`, `rayTopDistance`, `densities` |
| Multiple scattering | `atmos_multiscatter.slang` | `computeMain` — sphere integral + series sum |
| Sky-view | `atmos_skyview.slang` | `computeMain`, `hgPhase`, `rayleighPhase` |
| Cube fill | `atmos_skygen.slang` | `computeMain`, `dirToSkyViewUv`, sun disk |
| LUT alloc + dispatch | `renderer_detail.cppm` | `bakeEnvironment` — `useAtmosphere` branch |
| Source select + re-bake gate | `assets.cppm` · `renderer_lighting.cpp` | `renderScene`, `requestEnvBake` |
| Parameters + serde | `scene.cppm` | `AtmosphereSettings`, `atmosphereToJson` |

## Related

- [Procedural sky](../procedural-sky/) — the analytic gradient this replaces as the cube source
- [IBL bake pass](../ibl-bake-pass/) — runs the cube fill first, then the convolutions
- [Diffuse irradiance](../diffuse-irradiance/) — convolves the resulting cube for diffuse ambient
