# Phase 04 — Tangent basis (derivative-based)

**Status:** COMPLETED
**Depends on:** — (independent of 01–03)

> **Approach change (from the original "extend Vertex + .smesh v3" plan).** Growing the base
> `Vertex` to 48 B ripples through the skinning compute pre-pass (which deforms a *base 32-byte
> Vertex layout* into the deformed buffer), the previous-pose deformed buffer (motion vectors),
> `.smesh` save/load, and glTF/OBJ import + tangent generation — a high-risk migration with two
> code paths (static vs skinned). Instead we compute the **tangent basis per-fragment from screen-space
> derivatives** of world position + UV (Schüler's cotangent-frame method). This is **one code path**,
> **zero blast radius** (no `Vertex`/skinning/motion/import/save-load change), works identically for
> static and skinned meshes, and is a production-proven technique. Trade-off: the basis is derived from
> the interpolated geometry rather than a stored MikkTSpace tangent, so it can differ subtly from the
> basis a normal map was baked against (worst on mirrored UVs / hard edges). If a future asset needs
> exact MikkTSpace fidelity, a precomputed tangent **stream** (a second vertex binding, static meshes
> only) can be layered on without disturbing this path.

## Goal

Make tangent-space normal mapping possible by adding a derivative-based cotangent-frame helper to
`mesh.slang`, and carry the world position into `MaterialInput` so the helper has its inputs. The
helper is consumed by the normal-mapping path in phase 05.

## The helper (in `mesh.slang`)

```slang
// Per-fragment tangent frame from screen-space derivatives (no stored tangents needed).
float3x3 cotangentFrame(float3 N, float3 p, float2 uv)
{
    float3 dp1 = ddx(p);   float3 dp2 = ddy(p);
    float2 duv1 = ddx(uv); float2 duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = rsqrt(max(dot(T, T), dot(B, B)));
    return float3x3(T * invmax, B * invmax, N);
}

// Perturb the geometric normal N by a tangent-space normal-map sample (xyz in [-1,1]).
float3 perturbNormal(float3 N, float3 worldPos, float2 uv, float3 mapN)
{
    float3x3 tbn = cotangentFrame(N, worldPos, uv);
    return normalize(mul(mapN, tbn));
}
```

`MaterialInput` gains `float3 worldPos`, populated in `fragmentMain` from `input.worldPos`. The helpers
are defined this phase but not yet called (phase 05's normal path calls `perturbNormal`).

## Files to touch

- `engine/assets/shaders/mesh.slang` — add `cotangentFrame` + `perturbNormal`; add `worldPos` to
  `MaterialInput` and populate it in `fragmentMain`. No CPU-side change.

## Steps

1. Add the two helpers above `evalSurface`.
2. Add `float3 worldPos` to `MaterialInput`; set `mi.worldPos = input.worldPos` in `fragmentMain`.
3. Rebuild shaders; confirm the existing material e2e still passes (the helpers are unused, so output
   is unchanged).

## Gate / done

- `make engine` clean; material e2e unchanged (no behaviour change — helpers unused this phase).
- `make prepare-for-commit` clean. No `se`/docs concept change (internal shader helper).

## Risks

- **Derivative tangent vs baked basis** — documented above; acceptable for v1, refinement path noted.
- `ddx`/`ddy` are fragment-only (fine — the helper is fragment-side). They require the fragment to have
  valid derivatives (it does; this is the ordinary scene pass).
