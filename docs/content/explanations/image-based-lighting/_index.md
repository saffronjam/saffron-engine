+++
title = 'Image-based lighting'
weight = 9
+++

# Image-based lighting

The indirect, ambient term. Rather than a flat scalar, the engine convolves an environment into a diffuse irradiance cube and a roughness-prefiltered specular cube, and bakes the split-sum BRDF lookup table once at startup. The mesh shader samples all three for energy-conserving ambient light.

## Pages

| Page | Covers | Code |
|---|---|---|
| `ibl-overview` | the split-sum approximation, diffuse + specular, when it replaces flat ambient | `mesh.slang` · ambient block |
| `cubemaps-and-mips` | cube-compatible `Image` variants, 6 layers, mip chains, dual views | `renderer_detail.cppm` · cube images |
| `procedural-sky` | the analytic sky baked once (zenith/horizon gradient + sun disk) | `ibl_skygen.slang` |
| `diffuse-irradiance` | cosine-weighted hemisphere convolution into a small cube | `ibl_irradiance.slang` |
| `specular-prefilter` | GGX importance-sampled prefilter, one mip per roughness | `ibl_prefilter.slang` |
| `brdf-lut` | the 2D Fresnel scale/bias lookup table | `ibl_brdf.slang` |
| `ibl-bake-pass` | the one-time startup compute dispatch + caching | `renderer_detail.cppm` · IBL bake |
