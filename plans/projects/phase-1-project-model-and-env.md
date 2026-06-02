# Phase 1: Project Model And Env Startup

**Status:** COMPLETED

Completed 2026-06-02. Project metadata/state, env startup, app-data/userdata
resolution, project create/open/save control commands, auto-empty startup, and
legacy root `project.json` compatibility are implemented. Verified with toolbox
build plus headless control-plane smokes for `SAFFRON_AUTO_EMPTY_PROJECT=1` and
`SAFFRON_PROJECT=test-project`.

## Goal

Add durable project identity and project-root state, then make startup project
selection scriptable through environment variables. This phase should be enough
for tests and the CLI/editor to open or create a project without relying on file
dialogs.

## Project document

Extend `project.json` from a scene/catalog wrapper into a project document:

```json
{
  "version": 1,
  "name": "sample-project",
  "displayName": "Sample Project",
  "assets": [],
  "scene": {
    "version": 1,
    "entities": []
  }
}
```

`name` is the stable project id used for the folder. `displayName` is editable UI
text. Existing root `./project.json` files remain loadable by filling missing
metadata from the filename or parent directory.

## Engine behavior

- Track active project state in the host-side/editor context:
  - project root directory
  - active `project.json` path
  - project name
  - display name
- Set the asset server root to `<project-root>/assets` after a project is opened
  or created.
- Keep a compatibility path for the current root `./project.json` load so existing
  checkouts still start.
- When creating a new project, create the directory, `assets/models`,
  `assets/textures`, and a valid `project.json`.
- When opening a project, replace the scene, catalog, GPU asset caches, active
  project metadata, and editor selection.

## Environment startup

Implement startup precedence:

1. `SAFFRON_PROJECT`
2. `SAFFRON_AUTO_EMPTY_PROJECT=1`
3. no project loaded; wait for the editor to choose one

`SAFFRON_PROJECT` accepts:

- project name: resolve as `<appdata>/userdata/<project-name>/project.json`
- directory: open `<directory>/project.json`
- file: open that file and use its parent as the project root

`SAFFRON_AUTO_EMPTY_PROJECT=1` creates or opens a generated test project under
`appdata/userdata/`, bypassing the editor modal. Its name must be deterministic
enough for tests to find, but unique enough for parallel test runs when paired
with a per-run app-data override.

`SAFFRON_APPDATA_DIR` is available as a test/local override for the app-data root.
When unset, the engine uses repo-root `appdata`.

## Control surface

Add project-aware commands while preserving current save/load command names where
possible:

- `get-project`: returns loaded state, project path, root, name, and display name.
- `new-project`: creates and opens a project under app data or an explicit root.
- `open-project`: opens an existing project directory or file.
- `save-project`: saves the active project when no path is passed.

If `load-project` remains as an alias for compatibility, it should call the same
project-open path as `open-project`.

## Validation

- `SAFFRON_PROJECT=<name>` opens `appdata/userdata/<name>/project.json`.
- `SAFFRON_PROJECT=/path/to/project` opens `/path/to/project/project.json`.
- `SAFFRON_PROJECT=/path/to/project.json` opens that file.
- `SAFFRON_AUTO_EMPTY_PROJECT=1` starts without a modal dependency and produces a
  valid project.
- Existing root `./project.json` still loads.
- `get-project` reports unloaded state when no env project is selected and no
  compatibility root project was loaded.
