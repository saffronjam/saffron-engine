+++
title = 'Spot shadows'
weight = 2
+++

# Spot shadows

A spot light shadow reuses the directional path almost verbatim — the same depth pass, the same `pcfShadow` sampler — but with a perspective light view down the cone instead of an orthographic one. A spot has both a position and a direction, so its frustum is a true perspective view.

> [!NOTE]
> Only the first spot light in the scene is shadowed. Multiple shadowed spots would need a map (or atlas) per light plus per-light viewProj entries in the UBO.

## Light view and the depth pass

`renderScene` builds the frustum for the first spot it finds. The field of view is twice the cone's outer angle plus a couple of degrees of pad, so the penumbra at the cone edge stays inside the map; aspect is 1, and the far plane is the light's range. An `up`-vector flip avoids a degenerate `lookAt` when the spot points straight up or down. The transform goes to the renderer via `setSpotShadow`, which also records which light index is shadowed so the fragment knows where to apply it.

The pass is its own depth-only draw into a second 2048² map, declared in the graph next to the directional one. It calls the same `recordShadowDepth` — only the viewProj push constant differs. Same vertex-only pipeline, same instance set, same depth bias.

## Sampling per light

The spot shadow is applied inside the punctual light loop, but only for the matching light index:

```hlsl
if (globals.spotShadow.y != 0 && lightIndex == globals.spotShadow.x)
{
    shadow = pcfShadow(spotShadowMap, globals.spotShadowViewProj, worldPos);
}
```

`spotShadow.y` is the enable flag and `spotShadow.x` is the shadowed light's index. Every other spot and every point light contributes unshadowed. Visibility folds into the light's radiance alongside cone falloff and distance attenuation before the BRDF.

## Design and trade-offs

Sharing the depth pass keeps the spot shadow nearly free in code: one extra map, one pass declaration, one index check. A spot's perspective frustum makes the bias behave differently across the cone than the directional ortho does, which is why both maps share the same tuned [bias](../shadow-bias/) constants rather than per-light values. The single-spot path proves the mechanism; an array generalization is a later step.

## In the code

| What | File | Symbols |
|---|---|---|
| Build the perspective frustum | `assets.cppm` | `renderScene` (spot gather) |
| Store transform + light index | `renderer.cppm` | `setSpotShadow` |
| Add the pass | `renderer.cppm` | `beginFrameGraph` (`doSpotShadow`) |
| Record depth (shared) | `renderer_drawlist.cpp` | `recordShadowDepth` |
| Per-light sample + index check | `mesh.slang` | `punctual` (spot branch), `pcfShadow` |
| UBO fields | `renderer_lighting.cpp` | `spotShadowViewProj`, `spotShadow` |

## Related

- [Directional shadows](../directional-shadows/) — the depth path this reuses
- [PCF filtering](../pcf-filtering/) — the shared comparison kernel
- [Punctual lights and attenuation](../../lighting-and-brdf/punctual-lights-and-attenuation/) — the cone + range the shadow rides on
- [Render graph](../../frame-and-render-graph/render-graph-overview/) — where the pass slots in
