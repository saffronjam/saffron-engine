# Revert viewport presentation to native X11 child window

Restore the working pre-canvas state: engine renders to a swapchain and presents via a
native X11 window reparented over the webview. All viewport-presentation phase 1–3 changes
are uncommitted, so the revert is `git checkout HEAD -- <file>` per area, plus deleting
new untracked files and manually fixing files with mixed changes.

**Status:** COMPLETED

## Why

Phases 1–3 are implemented but the editor crashes immediately with
`Gdk-Message: Error 71 (Protocol error) dispatching to Wayland display` on every `make run`.
Root cause not yet pinpointed. The canvas path has never been observed showing a rendered
frame. Reverting restores a known-working editor.

---

## 1. Engine C++ — pure revert

```sh
git checkout HEAD -- engine/source/saffron/rendering/renderer.cppm
git checkout HEAD -- engine/source/saffron/rendering/renderer_detail.cppm
git checkout HEAD -- engine/source/saffron/rendering/renderer_types.cppm
git checkout HEAD -- engine/source/saffron/host/host.cppm
git checkout HEAD -- engine/source/saffron/control/control_commands_render.cpp
```

Restores swapchain transport, `presentViewportToSwapchain`, `attach-native-viewport` with
`XReparentWindow`, `resize-native-viewport` with `XMoveResizeWindow`. Removes
`ReadbackRing`, `ViewportTransport` enum, shm publisher, readback ring.

---

## 2. Rust bridge — pure revert

```sh
git checkout HEAD -- editor/src-tauri/src/lib.rs
git checkout HEAD -- editor/src-tauri/Cargo.toml      # restores raw-window-handle, removes memmap2
git checkout HEAD -- editor/src-tauri/Cargo.lock
git checkout HEAD -- editor/src-tauri/capabilities/default.json
```

Restores `GDK_BACKEND=x11`, `SDL_VIDEODRIVER=x11`, `attach_native_viewport`, `parent_xid`.
Removes `viewport://` custom protocol handler, shm mmap, `serve_viewport_frame`.
Capabilities: removes `allow-close`, `allow-is-maximized`, `allow-minimize`,
`allow-start-dragging`, `allow-toggle-maximize` (these were added for the custom titlebar
that came with `decorations: false`).

---

## 3. tauri.conf.json — pure revert

```sh
git checkout HEAD -- editor/src-tauri/tauri.conf.json
```

Removes `decorations: false`. Restores `minWidth: 900, minHeight: 620`.

---

## 4. Frontend — pure revert

```sh
git checkout HEAD -- editor/src/panels/ViewportPanel.tsx
```

Restores legacy div-only path, `PARKED_BOUNDS` occlusion, `attachViewport` on phase-ready.
Removes `ViewportMode`, canvas element, `useCanvasViewport` hook, WebGL blit, shm fetch.

---

## 5. New untracked files to DELETE

These files did not exist before and must be removed:

```sh
rm docs/content/explanations/ui-and-editor/tauri-editor-and-viewport-transport.md
rm editor/src/app/WindowTitlebar.tsx   # custom titlebar for decorations: false
```

Check whether `editor/src/app/ProjectMenu.tsx`, `editor/src/lib/flash.ts`, and
`editor/src/vite-env.d.ts` are viewport-related or unrelated editor improvements before
deleting — they may be worth keeping.

---

## 6. Deleted file to RESTORE

`tauri-editor-and-x11-bridge.md` was deleted; restore it:

```sh
git checkout HEAD -- docs/content/explanations/ui-and-editor/tauri-editor-and-x11-bridge.md
```

---

## 7. Docs — pure revert (viewport-specific)

```sh
git checkout HEAD -- docs/content/explanations/_index.md
git checkout HEAD -- docs/content/explanations/ui-and-editor/viewport-panel.md
git checkout HEAD -- docs/content/explanations/ui-and-editor/_index.md
git checkout HEAD -- docs/content/explanations/app-lifecycle-and-window/headless-and-capture.md
git checkout HEAD -- docs/content/how-to/build-and-run.md
```

Leave the following docs alone — they contain unrelated improvements:
`asset-pickers-and-drag-drop.md`, `editor-camera.md`, `gizmo.md`, `hierarchy-panel.md`,
`assets-panel-and-thumbnails.md`, `theme-and-fonts.md`, `control-plane-architecture.md`,
`render-commands.md`, `scene-commands.md`, `shared-types.md`, `json-gateway.md`,
`screenshots-and-capture.md`, `overview.md`, `reference/control-commands.md`.

---

## 8. AGENTS.md — manual edit (mixed changes)

`AGENTS.md` has TWO changes: the viewport description (revert) AND a "Concurrent edits"
convention section (keep). Do NOT `git checkout HEAD` this file. Instead edit manually:

- Restore the first paragraph to describe the X11 reparent bridge (not canvas/shm)
- Restore the editor section to say `SDL_VIDEODRIVER=x11` and `needs an X11/XWayland display`
- **Keep** the "Concurrent edits" bullet that was added (lines ~25-29)

Same applies to `editor/AGENTS.md` — restore the X11 reparent description but check for
any unrelated improvements worth keeping.

---

## 9. Rebuild and verify

```sh
make engine   # rebuilds the C++ engine with swapchain transport
make run
```

Expected engine log: `vulkan ready — gpu 'NVIDIA GeForce RTX 3070 Ti', 4 swapchain images`
(not `windowless readback ring`).

Expected result: the 3D viewport appears in the editor, no Wayland protocol error, the
editor is usable.

---

## What to keep regardless

- `Makefile` — keep `VK_ADD_DRIVER_FILES` (NVIDIA GPU) and `run-software` target
- `editor/vite.config.ts` — keep `server.watch.ignored` fix (prevents EINVAL on Linux)
- `plans/viewport-presentation/` — keep all plan files for future reference
- All engine files not listed above
- All other `editor/src/` files not listed above (CreateMenu, Layout, HierarchyPanel, etc.)
