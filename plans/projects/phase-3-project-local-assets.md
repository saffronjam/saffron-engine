# Phase 3: Project-Local Assets

**Status:** COMPLETED

Completed 2026-06-02. New imports write project-local meshes under
`assets/models` and textures under `assets/textures`, project create/open paths
ensure both directories exist, and import commands now require an active project
so runtime engine assets are not polluted. Legacy `meshes/...` mesh catalog paths
remain loadable, including when the file has been moved under `assets/models`.
Verified with toolbox build plus headless import smokes for model import, texture
import, project save, and legacy mesh-path thumbnail loading.

## Goal

Move imported asset outputs into the active project directory so projects are
self-contained and can be copied without depending on repo-root runtime assets.

## Asset layout

Imported assets should be stored under:

```text
<project-root>/assets/models/<uuid>.smesh
<project-root>/assets/textures/<uuid>.<ext>
```

Catalog paths remain relative to the project asset root:

```json
{
  "assets": [
    {
      "id": "3862017159553017004",
      "name": "cube",
      "type": "mesh",
      "path": "models/3862017159553017004.smesh"
    }
  ]
}
```

## Engine changes

- Change the asset server import target for meshes from `meshes/` to `models/`.
- Keep textures under `textures/`.
- Ensure model material textures imported during `import-model` are written under
  the active project asset root.
- Ensure every project create/open path creates `assets/models` and
  `assets/textures`.
- Resolve asset loads against the active project asset root.

## Compatibility

Old projects can contain `meshes/<uuid>.smesh`. Loading those paths must continue
to work. New imports and new saves should emit `models/<uuid>.smesh`.

Root runtime assets used by the engine itself, such as built-in cube glTF files,
remain engine assets. Imported project assets are project-local.

## Editor behavior

- Import Model writes project-local `.smesh` output.
- Import Texture writes project-local texture output.
- Asset browser paths should display the new relative paths without assuming
  repo-root `assets/`.

## Validation

- Importing a model creates a `.smesh` under `<project-root>/assets/models`.
- Importing a texture creates a file under `<project-root>/assets/textures`.
- Importing a model with an albedo texture writes that texture under
  `<project-root>/assets/textures`.
- Saving and reopening the project reloads meshes and textures from the project
  folder.
- Existing projects with `meshes/...` catalog entries still load.
