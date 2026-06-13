import { describe, expect, test } from "bun:test";
import { edgeZone, hitTestRects, insertionIndexForCenters, type LeafDropRect } from "./dockDrag";

function rect(left: number, top: number, width: number, height: number) {
  return { left, top, right: left + width, bottom: top + height, width, height };
}

const leaf: LeafDropRect = {
  leafId: "leaf:right",
  bodyRect: rect(0, 0, 100, 200),
  stripRect: rect(0, 0, 100, 30),
  stripCenters: [25, 75],
  acceptsTabs: true,
  acceptsSplits: false, // too small to split (half-width < MIN_SPLIT_PX) — exercises merge
};

describe("insertionIndexForCenters", () => {
  test("returns the index of the first center the pointer falls before", () => {
    expect(insertionIndexForCenters([25, 75], 10)).toBe(0);
    expect(insertionIndexForCenters([25, 75], 50)).toBe(1);
    expect(insertionIndexForCenters([25, 75], 90)).toBe(2);
  });

  test("empty strip (reveal band) always inserts at 0", () => {
    expect(insertionIndexForCenters([], 123)).toBe(0);
  });
});

describe("hitTestRects", () => {
  test("a pointer in the strip inserts at the computed index", () => {
    expect(hitTestRects([leaf], 50, 10)).toEqual({ kind: "tab", leafId: "leaf:right", index: 1 });
  });

  test("a pointer in the body (below the strip) merges (append index)", () => {
    expect(hitTestRects([leaf], 50, 100)).toEqual({ kind: "tab", leafId: "leaf:right", index: 2 });
  });

  test("a pointer outside every leaf resolves to null", () => {
    expect(hitTestRects([leaf], 500, 500)).toBeNull();
  });

  test("a non-accepting leaf is skipped", () => {
    expect(hitTestRects([{ ...leaf, acceptsTabs: false }], 50, 10)).toBeNull();
  });

  test("a reveal band (no strip) is a body-merge target at index 0", () => {
    const band: LeafDropRect = {
      leafId: "leaf:bottom",
      bodyRect: rect(0, 300, 400, 40),
      stripRect: null,
      stripCenters: [],
      acceptsTabs: true,
      acceptsSplits: false,
    };
    expect(hitTestRects([band], 200, 320)).toEqual({
      kind: "tab",
      leafId: "leaf:bottom",
      index: 0,
    });
  });

  test("a hovered strip wins over another leaf's body", () => {
    const other: LeafDropRect = {
      leafId: "leaf:bottom",
      bodyRect: rect(0, 0, 200, 400),
      stripRect: rect(0, 0, 200, 30),
      stripCenters: [50],
      acceptsTabs: true,
      acceptsSplits: false,
    };
    // Pointer at (50, 10): inside leaf's strip AND inside other's strip — first strip wins.
    expect(hitTestRects([leaf, other], 50, 10)).toEqual({
      kind: "tab",
      leafId: "leaf:right",
      index: 1,
    });
  });
});

describe("edgeZone / split drops", () => {
  // A leaf big enough that half its extent clears MIN_SPLIT_PX.
  const splitLeaf: LeafDropRect = {
    leafId: "leaf:big",
    bodyRect: rect(0, 0, 600, 400),
    stripRect: rect(0, 0, 600, 30),
    stripCenters: [],
    acceptsTabs: true,
    acceptsSplits: true,
  };

  test("the outer thirds pick a split edge, the center merges", () => {
    expect(edgeZone(splitLeaf.bodyRect, 30, 200)).toBe("left");
    expect(edgeZone(splitLeaf.bodyRect, 570, 200)).toBe("right");
    expect(edgeZone(splitLeaf.bodyRect, 300, 30)).toBe("top");
    expect(edgeZone(splitLeaf.bodyRect, 300, 370)).toBe("bottom");
    expect(edgeZone(splitLeaf.bodyRect, 300, 200)).toBeNull(); // center → merge
  });

  test("a too-small leaf never offers a split (half-extent < MIN_SPLIT_PX)", () => {
    const tiny = rect(0, 0, 100, 100);
    expect(edgeZone(tiny, 5, 50)).toBeNull();
  });

  test("hitTestRects returns a split target on an edge of a splittable leaf", () => {
    expect(hitTestRects([splitLeaf], 30, 200)).toEqual({
      kind: "split",
      leafId: "leaf:big",
      edge: "left",
    });
  });

  test("a non-splittable leaf merges on its edge instead of splitting", () => {
    const noSplit = { ...splitLeaf, acceptsSplits: false };
    expect(hitTestRects([noSplit], 30, 200)).toEqual({ kind: "tab", leafId: "leaf:big", index: 0 });
  });
});
