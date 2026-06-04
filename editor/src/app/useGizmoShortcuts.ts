/// Global viewport keyboard shortcuts. W/E/R map to the gizmo operation (translate
/// / rotate / scale), matching the C++ editor's W/E/R cycle (editor_gizmo.cpp); F
/// focuses the editor camera on the selection; Escape deselects.
///
/// INPUT MODEL (spike-0b, recorded in the phase-3 migration plan): the editor input
/// is **control-command-driven** — the webview owns the DOM and forwards intent to
/// the engine over the control socket. The engine renders windowless and gets no raw
/// keyboard from the webview, so the webview is the right place to bind W/E/R: it
/// sets `store.gizmo` optimistically and fires `set-gizmo` (mirroring the Topbar
/// buttons). The engine therefore never handles W/E/R itself, so this hook cannot
/// double-fire with it.
///
/// The handler is gated OFF while a text input / textarea / select / contentEditable
/// is focused, so typing a value (e.g. an entity name or a number field) never
/// retargets the gizmo.
import { useEffect } from "react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import { errorText } from "../lib/flash";
import type { GizmoState } from "../protocol";

type GizmoOp = GizmoState["op"];

const KEY_TO_OP: Record<string, GizmoOp> = {
  w: "translate",
  e: "rotate",
  r: "scale",
};

/// Log a rejected shortcut command. The hook is a global key listener with no panel
/// to anchor a flash banner, so the failure goes to the console rather than vanishing.
function logRejected(action: string, err: unknown): void {
  console.error(`${action} rejected:`, errorText(err));
}

/// True when the active element is a text-entry control, so shortcuts must not fire.
function isTextEntryFocused(): boolean {
  const el = document.activeElement;
  if (!el || !(el instanceof HTMLElement)) {
    return false;
  }
  if (el.isContentEditable) {
    return true;
  }
  const tag = el.tagName;
  return tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT";
}

export function useGizmoShortcuts(): void {
  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent): void => {
      // Let modified chords (Ctrl/Cmd/Alt) through — they belong to menus / the OS.
      if (event.ctrlKey || event.metaKey || event.altKey) {
        return;
      }
      if (isTextEntryFocused()) {
        return;
      }
      // Every shortcut here drives engine state, so only act once it is live.
      const store = useEditorStore.getState();
      if (store.engineStatus.phase !== "ready") {
        return;
      }

      const key = event.key.toLowerCase();

      const op = KEY_TO_OP[key];
      if (op) {
        event.preventDefault();
        // Optimistic local update + the command; the reconcile poll's get-gizmo read
        // keeps it in sync with any external mutation (e.g. `se set-gizmo`).
        store.setGizmo({ op });
        void client.setGizmo({ op }).catch((err: unknown) => logRejected("set-gizmo", err));
        return;
      }

      // F focuses the editor camera on the current selection.
      if (key === "f") {
        const selectedId = store.selectedId;
        if (selectedId === null) {
          return;
        }
        event.preventDefault();
        void client.focus(selectedId).catch((err: unknown) => logRejected("focus", err));
        return;
      }

      // Escape deselects: clear local selection immediately and tell the engine; the
      // reconcile poll confirms via selectionVersion.
      if (event.key === "Escape") {
        if (store.selectedId === null) {
          return;
        }
        event.preventDefault();
        store.setSelectedId(null);
        void client.deselect().catch((err: unknown) => logRejected("deselect", err));
        return;
      }
    };

    window.addEventListener("keydown", onKeyDown);
    return () => {
      window.removeEventListener("keydown", onKeyDown);
    };
  }, []);
}
