import { describe, expect, test } from "bun:test";
import type { MaterialGraph } from "../materials/graph";
import { graphsEqual } from "./MaterialGraphEditor";

// A small reference graph: two inputs (a constant + a texture slot) feeding a multiply, whose
// output drives materialOutput.baseColor. Enough nodes/edges to exercise the sorting both ways.
function baseGraph(): MaterialGraph {
  return {
    nodes: [
      { id: "c1", type: "constant", props: { value: [1, 0, 0, 1], editorPos: [60, 40] } },
      { id: "tex", type: "textureSlot", props: { slot: "albedo", editorPos: [60, 200] } },
      { id: "mul", type: "multiply", props: { editorPos: [300, 120] } },
      { id: "out", type: "materialOutput", props: { editorPos: [560, 120] } },
    ],
    edges: [
      { from: ["c1", "rgba"], to: ["mul", "a"] },
      { from: ["tex", "rgba"], to: ["mul", "b"] },
      { from: ["mul", "rgba"], to: ["out", "baseColor"] },
    ],
  };
}

describe("graphsEqual — order insensitivity", () => {
  test("a graph equals itself", () => {
    const g = baseGraph();
    expect(graphsEqual(g, g)).toBe(true);
  });

  test("equal graphs with nodes in a different order compare equal", () => {
    const a = baseGraph();
    const b = baseGraph();
    // Reverse the node array; identical node objects, just a different position.
    b.nodes.reverse();
    expect(graphsEqual(a, b)).toBe(true);
  });

  test("equal graphs with edges in a different order compare equal", () => {
    const a = baseGraph();
    const b = baseGraph();
    b.edges.reverse();
    expect(graphsEqual(a, b)).toBe(true);
  });

  test("both nodes and edges shuffled still compare equal", () => {
    const a = baseGraph();
    const b: MaterialGraph = {
      nodes: [a.nodes[2], a.nodes[0], a.nodes[3], a.nodes[1]],
      edges: [a.edges[1], a.edges[2], a.edges[0]],
    };
    expect(graphsEqual(a, b)).toBe(true);
  });

  test("comparison is symmetric", () => {
    const a = baseGraph();
    const b = baseGraph();
    b.nodes.reverse();
    b.edges.reverse();
    expect(graphsEqual(a, b)).toBe(graphsEqual(b, a));
  });

  test("two empty graphs are equal", () => {
    expect(graphsEqual({ nodes: [], edges: [] }, { nodes: [], edges: [] })).toBe(true);
  });
});

describe("graphsEqual — detecting differences", () => {
  test("a changed node prop value compares unequal", () => {
    const a = baseGraph();
    const b = baseGraph();
    // Change the constant's value: same id/type/keys, different data.
    (b.nodes[0].props as Record<string, unknown>).value = [0, 1, 0, 1];
    expect(graphsEqual(a, b)).toBe(false);
  });

  test("a changed texture slot prop compares unequal", () => {
    const a = baseGraph();
    const b = baseGraph();
    (b.nodes[1].props as Record<string, unknown>).slot = "emissive";
    expect(graphsEqual(a, b)).toBe(false);
  });

  test("a changed editorPos compares unequal (positions are part of the key)", () => {
    const a = baseGraph();
    const b = baseGraph();
    (b.nodes[2].props as Record<string, unknown>).editorPos = [301, 120];
    expect(graphsEqual(a, b)).toBe(false);
  });

  test("a changed node type compares unequal", () => {
    const a = baseGraph();
    const b = baseGraph();
    b.nodes[2].type = "add";
    expect(graphsEqual(a, b)).toBe(false);
  });

  test("an added edge compares unequal", () => {
    const a = baseGraph();
    const b = baseGraph();
    b.edges.push({ from: ["tex", "rgba"], to: ["out", "emissive"] });
    expect(graphsEqual(a, b)).toBe(false);
  });

  test("a removed edge compares unequal", () => {
    const a = baseGraph();
    const b = baseGraph();
    b.edges.pop();
    expect(graphsEqual(a, b)).toBe(false);
  });

  test("a re-targeted edge endpoint compares unequal", () => {
    const a = baseGraph();
    const b = baseGraph();
    // Same edge count, but the multiply output now drives roughness instead of baseColor.
    b.edges[2] = { from: ["mul", "rgba"], to: ["out", "roughness"] };
    expect(graphsEqual(a, b)).toBe(false);
  });

  test("a removed node compares unequal", () => {
    const a = baseGraph();
    const b = baseGraph();
    b.nodes.pop();
    expect(graphsEqual(a, b)).toBe(false);
  });

  test("an added node compares unequal", () => {
    const a = baseGraph();
    const b = baseGraph();
    b.nodes.push({ id: "uv1", type: "uv", props: { editorPos: [60, 360] } });
    expect(graphsEqual(a, b)).toBe(false);
  });

  test("a renamed node id compares unequal", () => {
    const a = baseGraph();
    const b = baseGraph();
    b.nodes[0].id = "c2";
    expect(graphsEqual(a, b)).toBe(false);
  });

  test("an empty graph differs from a non-empty one", () => {
    expect(graphsEqual({ nodes: [], edges: [] }, baseGraph())).toBe(false);
  });
});

describe("graphsEqual — prop presence vs absence", () => {
  test("a node with no props differs from the same node with props (key is structural)", () => {
    const withProps: MaterialGraph = {
      nodes: [{ id: "uv1", type: "uv", props: { editorPos: [0, 0] } }],
      edges: [],
    };
    const noProps: MaterialGraph = {
      nodes: [{ id: "uv1", type: "uv" }],
      edges: [],
    };
    // JSON.stringify drops an undefined `props`, so the serialized keys differ.
    expect(graphsEqual(withProps, noProps)).toBe(false);
  });

  test("two nodes lacking props compare equal", () => {
    const a: MaterialGraph = { nodes: [{ id: "uv1", type: "uv" }], edges: [] };
    const b: MaterialGraph = { nodes: [{ id: "uv1", type: "uv" }], edges: [] };
    expect(graphsEqual(a, b)).toBe(true);
  });
});
