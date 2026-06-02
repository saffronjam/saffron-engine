# Phase 2: Editor Startup And Recent Projects

**Status:** NOT STARTED

## Goal

Make the Tauri/React editor responsible for first-run project selection and recent
project management while the native host stays present-only.

## App data

Add Rust/Tauri helpers for app-data paths:

- local development resolves to repo-root `./appdata`
- user projects default to `./appdata/userdata`
- recent projects live in `./appdata/recent-projects.json`
- future packaged builds can swap the helper to a platform app-data directory

Add `appdata/` to `.gitignore`.

## Startup flow

On editor startup:

- If `SAFFRON_PROJECT` or `SAFFRON_AUTO_EMPTY_PROJECT=1` is set, pass the env
  through to the engine and do not show the project modal.
- Otherwise, start the host with no loaded project and show the project-selection
  modal once the control socket is available.
- While the modal is open, grey the editor background and disable normal editor
  interaction.
- After a project is opened or created, reset editor scene state and let the
  reconcile poll repopulate entities, selection, assets, environment, and stats.

## Modal behavior

The modal should support:

- recent projects list
- create project
- open existing project directory or `project.json`

Create project requires:

- project name
- display name

Validate the project name before sending it to the engine. The allowed form is
lowercase letters, digits, and hyphens, starting and ending with a letter or
digit.

## Recent projects

Store recent projects after successful create, open, and save:

```json
{
  "projects": [
    {
      "path": "/absolute/path/to/project/project.json",
      "name": "sample-project",
      "displayName": "Sample Project",
      "lastOpenedAt": "2026-06-02T00:00:00Z"
    }
  ]
}
```

Use absolute paths in the recents file. When a recent project no longer exists,
show it as unavailable or skip it during list loading; do not fail editor startup.

## File menu integration

- File -> Save Project saves the active project by default.
- File -> Save Project As can remain path-based if needed for compatibility.
- File -> Open Project opens via `open-project`, updates recents, and resets store
  state.
- Keep scene-only save/load separate from project open/save.

## Validation

- No env vars and no loaded project shows the modal over a greyed editor.
- Selecting a recent project opens it and dismisses the modal.
- Creating a project creates `project.json` plus `assets/models` and
  `assets/textures`.
- Invalid project names are rejected in the modal before the engine call.
- Env-selected and auto-empty startup paths do not show the modal.
