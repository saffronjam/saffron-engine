+++
title = 'Cluster indexing'
weight = 6
math = true
+++

# Cluster indexing

The [cull pass](../clustered-forward/) writes a light list per froxel. The fragment shader has
to find which froxel it is in to read the right list. That mapping — from a pixel position and
a view-space depth to a flat cluster index — is `clusterIndexFor` in `mesh.slang`, and it must
invert exactly the slicing the cull pass used.

## The three coordinates

A fragment's cluster is a triple $(x, y, z)$: a screen tile in X and Y, and a depth slice in Z.
The flat index packs them as

$$
\text{index} = x + y \cdot G_x + z \cdot G_x G_y
$$

the same encoding the cull pass unpacks. The X and Y tiles are a straight division of the pixel
position (`SV_Position.xy`) by the tile size, with a `min` that clamps the edge fragment so a
pixel exactly on the right or bottom border does not index one tile past the grid.

## The exponential Z slice

The depth slice is the part that has to match the cull pass's exponential planes. Given a
view-space depth $d = \max(-z_\text{view}, n)$ (negate the view-Z and floor at the near plane),
the slice is the logarithmic inverse of $z_i = -n(f/n)^{i/N}$:

$$
\text{slice} = \left\lfloor \frac{\ln(d / n)}{\ln(f / n)} \cdot N \right\rfloor
$$

```hlsl
float depth = max(-viewZ, near);
uint zSlice = uint(log(depth / near) / log(far / near) * float(clusterParams.gridSize.z));
zSlice = min(zSlice, clusterParams.gridSize.z - 1);
```

The $\log$ exactly undoes the $\text{pow}$ the cull used. Slicing on screen-space (NDC) depth
instead of view-space would put fragments in a different slice than the one the cull assigned
their light to, and lights near slice boundaries would pop. The two formulas are deliberately
mirror images.

The fragment first needs `viewZ`, which it gets by transforming its world position by the
cached view matrix, then loops its cluster's lights:

```hlsl
float viewZ = mul(clusterParams.view, float4(input.worldPos, 1.0)).z;
uint clusterIndex = clusterIndexFor(input.position.xy, viewZ);
uint count = clusters[clusterIndex].count;
for (uint i = 0; i < count; i = i + 1)
    lo += punctual(lights[clusters[clusterIndex].indices[i]], ..., albedo, metallic, roughness);
```

## In the code

| What | File | Symbols |
|---|---|---|
| Pixel + view-Z → flat index | `mesh.slang` | `clusterIndexFor` |
| View-Z transform + the loop | `mesh.slang` | `fragmentMain` — `clusterParams.view`, `clusters[...]` |
| Matching forward slicing | `light_cull.slang` | `computeMain` — `tileNear`/`tileFar` `pow` |
| Grid dims + z planes upload | `renderer_lighting.cpp` | `setClusterCamera` — `gridSize`, `zPlanes` |

> [!TIP]
> The Z slice uses view-space depth, not the rasterizer's NDC depth. The $\log$/$\text{pow}$
> pair across the index and cull shaders must use the same `near`/`far`, or fragments and their
> lights land in different slices.

## Related

- [Clustered forward](../clustered-forward/) — the cull pass this inverts
- [Per-cluster cap](../per-cluster-cap/) — what bounds the `count` this loop reads
- [Punctual lights and attenuation](../punctual-lights-and-attenuation/) — what the loop calls per light
