# Projects Plan

Introduce first-class Saffron projects: startup project selection, recent projects,
local app-data storage, environment-variable test hooks, project metadata, and
project-local imported assets.

## Status convention

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` /
`COMPLETED`). Mark a phase `COMPLETED` when its work is done and validation-clean;
delete a phase file only after it is `COMPLETED` and merged.

## Target shape

During local development, editor app data lives in repo-root `./appdata/`. That
directory is runtime state and must be gitignored.

User-created local projects default to:

```text
appdata/userdata/<project-name>/
  project.json
  assets/
    models/
    textures/
```

The project name is the stable folder-safe id. It uses lowercase letters, digits,
and hyphens, and starts and ends with a letter or digit. The project file also
stores a separate `displayName`, which is the user-facing label.

The host remains a present-only viewport process. Startup project selection,
recent projects, and the grey modal overlay belong to the Tauri/React editor.

## Startup precedence

Project startup is intentionally simple for tests:

1. `SAFFRON_PROJECT` selects a project without showing the modal.
2. `SAFFRON_AUTO_EMPTY_PROJECT=1` creates or opens a generated empty test project.
3. If neither env path chooses a project, the editor shows the project-selection modal.

`SAFFRON_PROJECT` accepts a project name, a project directory, or a direct
`project.json` path. A project name resolves under `appdata/userdata/`.

## Phase map

| # | Phase | File | Depends on |
|---|-------|------|------------|
| 1 | Project model + env startup | `phase-1-project-model-and-env.md` | - |
| 2 | Editor startup modal + recents | `phase-2-editor-startup-and-recents.md` | 1 |
| 3 | Project-local assets | `phase-3-project-local-assets.md` | 1 |
| 4 | Tests, docs, and migration | `phase-4-tests-docs-and-migration.md` | 1-3 |

## Current anchors

- The host currently auto-loads root `project.json` if it exists and otherwise
  seeds a cube scene in `Saffron.Host`.
- `AssetServer` currently roots at copied engine assets and imports meshes under
  `meshes/` and textures under `textures/`.
- The Tauri backend already owns engine spawning and can pass startup environment
  variables before the host process starts.
- The React editor already has file-menu save/load calls and store reset logic
  after a project or scene load.
