+++
title = 'Diffuse irradiance'
weight = 4
math = true
+++

# Diffuse irradiance

The diffuse half of IBL asks: given a surface with normal $n$, how much light arrives from the whole environment, cosine-weighted? That's the irradiance integral. It depends only on $n$ — a Lambertian surface scatters incoming light equally in all directions — so it precomputes into a small cube indexed by normal. One convolution at bake time, a single fetch per fragment.

## The integral

Irradiance is the environment integrated over the hemisphere around $n$, weighted by the cosine $\cos\theta = n\cdot l$:

$$
E(n) = \int_\Omega L_i(l)\, (n \cdot l)\, dl
$$

The Lambertian BRDF turns irradiance into outgoing diffuse radiance by $\rho/\pi$, which is why the mesh shader multiplies the sampled irradiance by `albedo` (and the $1/\pi$ is folded in at bake — below).

## How the convolution samples the hemisphere

Each output texel is a normal $n$. The shader builds a tangent frame around it, then walks a regular grid in spherical coordinates: 64 steps in azimuth $\phi$, 16 in zenith $\theta$. Each sample is weighted by `cos(theta) * sin(theta)`.

```hlsl
float3 sampleDir = tangent.x * right + tangent.y * up + tangent.z * n;
irradiance += envCube.SampleLevel(sampleDir, 0.0).rgb * cos(theta) * sin(theta);
```

The $\cos\theta$ is the Lambert cosine; the $\sin\theta$ is the Jacobian of the spherical parameterization — without it, samples bunch up near the pole and over-count. A `+ 0.5` offset puts samples at cell centers.

## Normalizing the sum

A uniform-grid average isn't a Monte-Carlo estimator of the integral, so the result is scaled to keep energy right:

```hlsl
irradiance = PI * irradiance / float(samples);
```

Dividing by the sample count averages; the leading $\pi$ accounts for the analytic value of $\int_\Omega \cos\theta\sin\theta\,d\theta\,d\phi$ over the hemisphere. The net effect: the stored irradiance, later divided by $\pi$ in the Lambertian term and multiplied by albedo, gives correct diffuse magnitude. The mesh shader writes `kd * irradiance * albedo` — the $1/\pi$ already lives in this bake constant, so it isn't applied again.

## Building the tangent frame

The convolution needs an orthonormal basis $(\text{right}, \text{up}, n)$ to orient the hemisphere. The shader picks an up vector not parallel to $n$, then Gram-Schmidt:

```hlsl
float3 up    = abs(n.y) < 0.999 ? float3(0,1,0) : float3(1,0,0);
float3 right = normalize(cross(up, n));
up = cross(n, right);
```

The `0.999` guard swaps to a different reference axis near the poles, where the world up would be degenerate — exactly the sky-poles of a cube.

## Why a tiny cube is enough

The cosine convolution is a heavy low-pass filter; it wipes out all high-frequency detail, so the output is smooth no matter how sharp the environment is. A `32²` cube captures it with no visible loss, which is why [the bake](../ibl-bake-pass/) keeps it that small. The whole convolution is `32 × 32 × 6 × 1024` environment samples, done once at startup.

## In the code

| What | File | Symbols |
|---|---|---|
| Convolution + tangent frame | `ibl_irradiance.slang` | `computeMain`, `phiSteps`/`thetaSteps`, `PI / samples` normalize, `up`/`right` |
| Bindings | `ibl_irradiance.slang` | `envCube` (sampler), `outCube` (rgba16f storage) |
| Cube size | `renderer_detail.cppm` | `IblIrradianceSize` (32) |
| Consumed as diffuse | `mesh.slang` | `fragmentMain` — `kd * irradiance * albedo` |

## Related

- [Procedural sky](../procedural-sky/) — the environment being convolved
- [Specular prefilter](../specular-prefilter/) — the specular counterpart
- [Cook-Torrance BRDF](../../lighting-and-brdf/cook-torrance-brdf/) — the Lambertian diffuse term this feeds
