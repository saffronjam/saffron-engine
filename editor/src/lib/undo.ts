/// The pure per-tab undo/redo state machine: an `UndoableEdit` record and a
/// `TabHistory` value with DOM-free, async-free transforms. The store layers the
/// async replay (`await edit.undo()`) and the per-tab dictionary on top; nothing
/// here touches React, the control client, or a promise.

/// One reversible edit: the closures that revert and re-apply it, plus the context
/// to restore. The store awaits `undo`/`redo`; the pure functions never call them.
export interface UndoableEdit {
  /// Short human phrase for the Edit menu and undo affordance ("Move", "Set albedo").
  label: string;
  /// Replays the inverse (or loads the prior snapshot for a snapshot tab). The store
  /// awaits it and ignores the resolved value (a control call's echo).
  undo: () => Promise<unknown>;
  /// Re-applies the edit.
  redo: () => Promise<unknown>;
  /// The entity (or material) the edit acted on, restored as selection context on
  /// replay. Opaque string id — never `Number()` it.
  selectionId?: string;
  /// `false` marks an edit that can be undone but not redone (entity creation, whose
  /// re-creation would mint a new id); its undo also clears the redo branch above it.
  /// Defaults to redoable.
  redoable?: boolean;
}

/// One tab's history: `past` newest-last (its tail is the next undo), `future` the
/// redo branch (its head is the next redo).
export interface TabHistory {
  past: UndoableEdit[];
  future: UndoableEdit[];
}

/// Per-tab cap on retained entries; the oldest `past` entry is dropped past it.
export const HISTORY_CAP = 200;

export function emptyHistory(): TabHistory {
  return { past: [], future: [] };
}

export function canUndo(history: TabHistory): boolean {
  return history.past.length > 0;
}

export function canRedo(history: TabHistory): boolean {
  const next = history.future[0];
  return next !== undefined && next.redoable !== false;
}

export function undoLabel(history: TabHistory): string | null {
  return history.past.at(-1)?.label ?? null;
}

export function redoLabel(history: TabHistory): string | null {
  return canRedo(history) ? (history.future[0]?.label ?? null) : null;
}

/// Append `edit` to `past`, clear the redo branch, and drop the oldest entry past
/// the cap. A fresh edit always abandons `future` (the linear-history model).
export function appendEdit(history: TabHistory, edit: UndoableEdit): TabHistory {
  const past = [...history.past, edit];
  if (past.length > HISTORY_CAP) {
    past.splice(0, past.length - HISTORY_CAP);
  }
  return { past, future: [] };
}

/// Pop the next undo (the tail of `past`). A redoable edit moves to the front of
/// `future`; a non-redoable one is dropped and the redo branch cleared (its redo is
/// unreachable once undone). `null` when `past` is empty.
export function takeUndo(history: TabHistory): { edit: UndoableEdit; next: TabHistory } | null {
  const edit = history.past.at(-1);
  if (edit === undefined) {
    return null;
  }
  const past = history.past.slice(0, -1);
  const future = edit.redoable === false ? [] : [edit, ...history.future];
  return { edit, next: { past, future } };
}

/// Pop the next redo (the head of `future`) back onto `past`. `null` when there is
/// no redoable next entry.
export function takeRedo(history: TabHistory): { edit: UndoableEdit; next: TabHistory } | null {
  const edit = history.future[0];
  if (edit === undefined || edit.redoable === false) {
    return null;
  }
  return { edit, next: { past: [...history.past, edit], future: history.future.slice(1) } };
}
