# Phase 4: Tests, Docs, And Migration

**Status:** COMPLETED

Completed 2026-06-02. Added the `ProjectInfo` control schema and regenerated
protocol types; extended the live control-schema contract test for project
commands; added a project startup/import compatibility smoke and wired it into
`tools/ci/check.sh`; updated project/import/control docs; and verified root
`appdata/` remains ignored. `tools/ci/check.sh` and `docs` Hugo build both pass.

## Goal

Close the project feature with reproducible checks, schema updates, documentation,
and compatibility coverage for existing root project files.

## Tests

Add or extend checks for:

- `SAFFRON_PROJECT` by project name, directory, and file path.
- `SAFFRON_AUTO_EMPTY_PROJECT=1` startup.
- invalid project names.
- project creation directory shape.
- recent-projects persistence.
- project-local model and texture imports.
- old root `./project.json` compatibility.
- active-project save without an explicit path.

Prefer the existing `tools/ci/check.sh` flow where practical: engine build,
present-only smoke, control-schema contract test, and frontend build.

## Schema and protocol

If project commands return structured DTOs, add schemas for them under
`schemas/control/` and regenerate `editor/src/protocol/index.ts` through
`bun run gen:protocol`.

Extend the control-schema contract test so live project command output matches
the schemas. Keep u64 ids string-safe on the TypeScript side.

## Documentation

Add a docs explanation page for projects covering:

- project directory layout
- `project.json` metadata
- project-local assets
- startup env vars for tests
- recent-project storage during local development

Update the relevant editor/file-ops docs and the docs hub row.

## Migration notes

- Existing root `./project.json` remains loadable.
- Old catalog paths under `meshes/` remain loadable.
- New projects and new imports use `assets/models` and `assets/textures`.
- `appdata/` is local runtime data and is ignored by git.

## Validation

- `tools/ci/check.sh` passes in the `saffron-build` toolbox.
- `bun run check` regenerates protocol types cleanly.
- A fresh checkout with no env vars shows the modal path.
- Env-driven startup is usable in automated tests without file dialogs.
