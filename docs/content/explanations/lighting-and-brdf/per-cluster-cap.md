+++
title = 'Per-cluster cap'
weight = 10
+++

# Per-cluster cap

Each froxel stores its light list in a fixed-size array, so a cluster can hold at most 64
lights. When more than 64 punctual lights overlap one froxel, the
[cull pass](../clustered-forward/) keeps the first 64 and silently drops the rest.

## A fixed array per cluster

The cap is `MAX_LIGHTS_PER_CLUSTER = 64`, declared in `light_cull.slang`, `mesh.slang`, and
`renderer_detail.cppm` (as `MaxLightsPerCluster`). The cluster struct is a `count` plus a fixed
array of that many indices:

```hlsl
struct Cluster
{
    uint count;
    uint indices[MAX_LIGHTS_PER_CLUSTER];   // 64
};
```

The whole cluster buffer is sized `ClusterCount * ClusterStride`, where
`ClusterStride = sizeof(u32) * (1 + MaxLightsPerCluster)` — one count word plus 64 index words
per froxel. A fixed stride keeps the buffer a flat, GPU-friendly allocation with no
indirection, at the cost of a hard ceiling.

## The silent drop

The cull loop appends a light only while there is room. Past 64 it stops writing but keeps
scanning, so further overlapping lights are simply never recorded:

```hlsl
if (dot(delta, delta) <= radius * radius)
{
    if (count < MAX_LIGHTS_PER_CLUSTER)   // the cap
    {
        clusters[clusterIndex].indices[count] = i;
        count = count + 1;
    }
}
```

There is no warning and no spill list. The fragment shader reads `count` and loops exactly that
many. Beyond the cap, the lights that survive are whichever ones the cull reached first — lowest
scene index, not nearest or brightest. A 65th overlapping light produces no lighting in that
froxel.

## When it bites

For typical scenes the cap is invisible: 64 lights touching one $16\times9\times24$ froxel is a
lot of overlap. It matters when many large-`range` lights pile into the same view region — a
dense cluster of point lights, or a few oversized ranges that each land in most froxels.

This is also the one case where the clustered path and the
[brute-force fallback](../brute-force-fallback/) can diverge. Below the cap the two are
pixel-identical, because every light the clustered loop skips contributes exactly zero. Above
the cap, the clustered loop drops lights the brute-force loop still sees. If you suspect the
cap, `se set-clustered 0` reveals the difference.

## In the code

| What | File | Symbols |
|---|---|---|
| Cap + array | `light_cull.slang` | `MAX_LIGHTS_PER_CLUSTER`, `Cluster`, the `count <` guard |
| Matching shader-side cap | `mesh.slang` | `MAX_LIGHTS_PER_CLUSTER`, `Cluster` |
| Buffer sizing | `renderer_detail.cppm` | `MaxLightsPerCluster`, `ClusterStride`, `ClusterCount` |

> [!TIP]
> Raising the cap means changing `MAX_LIGHTS_PER_CLUSTER` in both shaders and
> `MaxLightsPerCluster` in `renderer_detail.cppm` together. The cluster buffer is
> `ClusterStride`-sized off the C++ constant, so a mismatch corrupts the per-froxel stride.

## Related

- [Clustered forward](../clustered-forward/) — the cull pass that fills the list
- [Cluster indexing](../cluster-indexing/) — how the fragment reads the bounded `count`
- [Brute-force fallback](../brute-force-fallback/) — the path that ignores the cap entirely
