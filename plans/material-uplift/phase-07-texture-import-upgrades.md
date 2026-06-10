# Phase 07 — Texture import upgrades

**Status:** NOT STARTED
**Depends on:** 03

## Goal

Make real downloaded PBR sets decode correctly: add **EXR** decoding (tinyexr), **16-bit PNG**
decoding (`stbi_load_16`), record **colorspace** and **normal-convention** intent on the
`AssetEntry`, and normalize **DX→GL normals** and **gloss→roughness** at import so the übershader
stays branch-free.

## Why

The decode path is stb-only today: `decodeImage`/`decodeImageFromMemory` (RGBA8), `decodeImageHdr`
(`stbi_loadf`), no EXR, no `stbi_load_16`. The Poly Haven coast-rocks asset ships normal + roughness
as **EXR** (won't decode at all) and displacement as **16-bit PNG** (silently truncates to 8-bit).
Colorspace and normal-convention decisions are unrecoverable later, so they must be set at import.

## Design

- **EXR**: vendor **tinyexr** (single-header, like stb) via `cmake/Dependencies.cmake` + an impl TU under
  `cmake/`. Add `decodeImageExr(path|bytes) -> DecodedImageFloat`. Route `.exr` to it; keep stb for the rest.
  If you'd rather not vendor yet, fail gracefully with a clear "EXR unsupported — convert to .hdr/.png".
- **16-bit PNG**: add `decodeImage16(path|bytes) -> DecodedImage16` (RGBA u16 via `stbi_load_16`); used for
  height/displacement so parallax (phase 06) gets smooth gradients without terracing. Upload as
  `eR16Unorm`/`eR16G16B16A16Unorm` (a new `uploadTexture16` mirroring `uploadTextureFloat`).
- **Metadata on `AssetEntry`**: `hdr`/`linear` already exist (pick sRGB vs UNORM). Add a `normalConvention`
  (or a small `textureRole` enum) so the importer can record "this is a GL normal" and the bake step / shader
  reads it. DX→GL is baked at import (invert green) so the runtime never branches on convention.
- **Gloss→roughness**: if a map is tagged glossiness, invert (`1-x`) at import into a roughness map; never
  carry a second runtime workflow.

## Files to touch

- `cmake/Dependencies.cmake` (+ a `cmake/tinyexr_impl.cpp` TU) — vendor tinyexr.
- `engine/source/saffron/geometry/geometry.cppm` — `decodeImageExr`, `decodeImage16`; route by extension.
- `engine/source/saffron/rendering/renderer_textures.cpp` — `uploadTexture16` (UNORM 16-bit format).
- `engine/source/saffron/scene/scene.cppm` — `AssetEntry` gains `normalConvention`/role metadata;
  update `catalogToJson`/`FromJson` (in `assets.cppm`) for the new field.
- `engine/source/saffron/assets/assets.cppm` — `registerTextureBytes`/`registerHdrTextureBytes` gain a
  role/colorspace/convention parameter; an import-time DX→GL invert + gloss→rough invert helper;
  `loadTextureAsset` branches on EXR/16-bit by entry metadata.

## Steps

1. Vendor tinyexr; add `decodeImageExr` + a smoke decode of the coast-rocks normal EXR.
2. Add `decodeImage16` + `uploadTexture16`; decode the 16-bit displacement PNG without truncation.
3. Add the `AssetEntry` metadata field + serde; thread colorspace/convention through register/upload.
4. Add the DX→GL and gloss→rough bake-at-import helpers (operate on the decoded buffer before upload).
5. Verify: the four coast-rocks files all import and the entry metadata is correct (sRGB diff, linear rest).

## Gate / done

- `make engine` clean; all four `coast_sand_rocks_02` textures import with correct format/colorspace.
- 16-bit displacement keeps its precision (spot-check a histogram or a known pixel).
- `make prepare-for-commit` clean. Docs: texture import + colorspace rules.

## Risks

- **tinyexr build** under clang21/libc++ + the no-`import std` rule for C++-header-wrapping modules
  (geometry already uses classic `#include` in the GMF — tinyexr fits there). Use the same impl-TU pattern
  as stb/vma in `cmake/`.
- **Format support**: 16-bit UNORM single/RGBA must be device-supported (it is on llvmpipe + real GPUs;
  check the `vk::Format` feature flags).
- Don't over-engineer the role enum now — the importer (phase 08) is the real consumer; record just
  enough (colorspace + normal convention) to make decisions unrecoverable-safe.
