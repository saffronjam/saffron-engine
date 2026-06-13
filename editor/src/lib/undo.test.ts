import { describe, expect, test } from "bun:test";
import {
  appendEdit,
  canRedo,
  canUndo,
  emptyHistory,
  HISTORY_CAP,
  redoLabel,
  takeRedo,
  takeUndo,
  undoLabel,
  type UndoableEdit,
} from "./undo";

/// A trivial edit whose thunks are no-ops — the pure state machine never calls them.
function edit(label: string, redoable?: boolean): UndoableEdit {
  return {
    label,
    undo: async () => {},
    redo: async () => {},
    ...(redoable === undefined ? {} : { redoable }),
  };
}

describe("appendEdit", () => {
  test("appends to past and clears the redo branch", () => {
    const undone = takeUndo(appendEdit(emptyHistory(), edit("a")))!;
    // After undo, `a` is on future; a fresh edit must abandon that redo branch.
    const next = appendEdit(undone.next, edit("b"));
    expect(next.past.map((e) => e.label)).toEqual(["b"]);
    expect(next.future).toEqual([]);
  });

  test("drops the oldest entry past the cap, keeping the newest", () => {
    let history = emptyHistory();
    for (let i = 0; i < HISTORY_CAP + 5; i++) {
      history = appendEdit(history, edit(`e${i}`));
    }
    expect(history.past.length).toBe(HISTORY_CAP);
    expect(history.past[0]?.label).toBe("e5");
    expect(history.past.at(-1)?.label).toBe(`e${HISTORY_CAP + 4}`);
  });
});

describe("takeUndo / takeRedo", () => {
  test("move the right edit between stacks and return that edit", () => {
    const history = appendEdit(appendEdit(emptyHistory(), edit("a")), edit("b"));
    const undo = takeUndo(history)!;
    expect(undo.edit.label).toBe("b");
    expect(undo.next.past.map((e) => e.label)).toEqual(["a"]);
    expect(undo.next.future.map((e) => e.label)).toEqual(["b"]);

    const redo = takeRedo(undo.next)!;
    expect(redo.edit.label).toBe("b");
    expect(redo.next.past.map((e) => e.label)).toEqual(["a", "b"]);
    expect(redo.next.future).toEqual([]);
  });

  test("return null on an empty stack", () => {
    expect(takeUndo(emptyHistory())).toBeNull();
    expect(takeRedo(emptyHistory())).toBeNull();
  });
});

describe("redoable flag", () => {
  test("a non-redoable undo drops the entry and clears the redo branch", () => {
    // past: [a, create]; undo `create` (non-redoable) must not reappear on future,
    // and the future branch above it is unreachable, so it clears.
    const history = appendEdit(appendEdit(emptyHistory(), edit("a")), edit("create", false));
    const undo = takeUndo(history)!;
    expect(undo.edit.label).toBe("create");
    expect(undo.next.future).toEqual([]);
    expect(canRedo(undo.next)).toBe(false);
    expect(takeRedo(undo.next)).toBeNull();
  });
});

describe("derived state", () => {
  test("canUndo / canRedo / labels track the stacks", () => {
    const empty = emptyHistory();
    expect(canUndo(empty)).toBe(false);
    expect(canRedo(empty)).toBe(false);
    expect(undoLabel(empty)).toBeNull();
    expect(redoLabel(empty)).toBeNull();

    const history = appendEdit(empty, edit("a"));
    expect(canUndo(history)).toBe(true);
    expect(undoLabel(history)).toBe("a");
    expect(canRedo(history)).toBe(false);

    const undone = takeUndo(history)!.next;
    expect(canUndo(undone)).toBe(false);
    expect(canRedo(undone)).toBe(true);
    expect(redoLabel(undone)).toBe("a");
  });
});
