# Phase 15 — Engine hardening

**Status:** NOT STARTED
**Depends on:** 05

## Goal

Make the system survive production texture sets: **bindless slot reclamation** (a free-list, since slots
are never reused today), **mipmap generation** (+ optional BC compression) for sampled textures, and
**dangling-reference validation** on scene/material load (warn + default, never silently null).

## Why

A full PBR set is up to ~5 textures/material; with a 1024-slot bindless pool and **no reclamation**
(`nextBindlessIndex++`, never freed — `renderer_textures.cpp`), a material-heavy or hot-reloaded scene
exhausts the pool. And 4K textures uploaded at **1 mip** alias badly (visible in the phase-12 preview too).
These are real correctness/quality gaps, orthogonal to the feature work, batched here.

## Design

- **Slot reclamation**: a free-list of bindless indices. On `GpuTexture` destroy, return its index to the
  free-list; `nextBindlessIndex` only grows when the free-list is empty. Guard against returning a slot a
  live material still references — safe because resolve holds `Ref<GpuTexture>` for the frame; reclaim only
  when the last `Ref` drops. Write a stale/default descriptor into a reclaimed slot before reuse.
- **Mipmaps**: generate a full mip chain on upload (`vkCmdBlitImage` down-chain, or a compute downsample),
  and sample with the existing trilinear sampler. Needed for minified surfaces + preview/thumbnail quality.
- **BC compression** (optional, can defer): transcode to BC7 (color) / BC5 (normal) at import for memory +
  bandwidth. Larger task; gate behind a flag and land mipmaps first.
- **Dangling-ref validation**: on scene load and material resolve, a referenced `Uuid` not in the catalog →
  log once + bind the default material/texture (phase 03/09), never a null `Ref`/black.

## Files to touch

- `engine/source/saffron/rendering/renderer_textures.cpp` / `renderer_types.cppm` — the bindless free-list
  on `Descriptors`; reclaim on `GpuTexture` teardown; mip generation in `uploadTexture`/`uploadTextureFloat`/
  `uploadTexture16`.
- `engine/source/saffron/assets/assets.cppm` — validation in `loadScene`/`resolveEntityMaterials`/
  `resolveMaterialAsset`; (optional) BC transcode in the register/import path.
- `engine/source/saffron/scene/scene.cppm` — (if needed) a catalog-presence check helper.

## Steps

1. Add the bindless free-list + reclaim-on-destroy; test by importing/deleting many textures and asserting
   the index count stays bounded.
2. Generate mipmaps on upload; verify minified surfaces stop shimmering (and the preview looks crisp).
3. Add dangling-ref validation → default + one warning; test by deleting a referenced texture.
4. (Optional) BC7/BC5 transcode behind a flag.

## Gate / done

- `make engine` clean; importing+deleting 100 textures keeps the bindless index bounded (free-list reused).
- Mipmapped 4K textures don't alias under minification. Missing refs → default + warn, never black/crash.
- `make prepare-for-commit` clean. Docs: bindless lifetime + mipmaps note.

## Risks

- **Reclaim-too-early** is a use-after-free of a descriptor slot: only reclaim when the last `Ref` drops and
  the GPU is done with frames that used it (respect frames-in-flight). Conservative: reclaim during
  `waitGpuIdle`/project unload first, then refine to per-frame.
- **Mip gen format support**: blit requires `BLIT_SRC/DST` format features; check per format (16-bit/EXR).
- BC transcode is the big-ticket item — keep it optional so the phase lands without it.
