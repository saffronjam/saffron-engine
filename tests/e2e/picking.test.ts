// Pick-ray convention over the control plane: viewport UV (0,0 = top-left) maps into
// the renderer's y-down clip space, so an entity in the upper half of the screen picks
// at v < 0.5. Guards against a double y-flip mirroring the ray about screen center
// (clicking above an object selected it, clicking on it missed).

import { afterAll, beforeAll, expect, test } from "bun:test";
import { Engine } from "./harness.ts";

let engine: Engine;
beforeAll(async () => {
  engine = await Engine.boot({ SAFFRON_AUTO_EMPTY_PROJECT: "1" });
  // yaw 0 / pitch 0 looks down -Z, so world +Y projects screen-up.
  await engine.call("set-camera", { yaw: 0, pitch: 0 });
});
afterAll(async () => {
  await engine?.shutdown();
});

interface Ref {
  id: string;
  name: string;
}
interface PickResult {
  hit: boolean;
  id?: string;
  name?: string;
  kind?: string;
}

test("an entity in the upper half of the screen picks at v < 0.5", async () => {
  const cube = await engine.call<Ref>("add-entity", { args: ["cube"] });
  await engine.call("rename-entity", { entity: cube.id, name: "p-cube" });
  await engine.call("set-transform", { entity: cube.id, translation: { x: 0, y: 0, z: 0 } });
  await engine.call("focus", { entity: cube.id });
  await engine.settle();

  // Centered after the focus: the symmetric center pick lands either way.
  const centered = await engine.call<PickResult>("pick", {});
  expect(centered.hit).toBe(true);
  expect(centered.id).toBe(cube.id);

  // Raise the cube one unit: world +Y is screen-up, so it now sits above center.
  await engine.call("set-transform", { entity: cube.id, translation: { x: 0, y: 1, z: 0 } });
  await engine.settle();

  // Scan down from just above center until the cube answers. A mirrored ray would
  // only hit in the lower half, so the scan failing means the convention regressed.
  let hitV = 0;
  for (let v = 0.46; v >= 0.1; v -= 0.04) {
    const r = await engine.call<PickResult>("pick", { u: 0.5, v });
    if (r.hit && r.id === cube.id) {
      hitV = v;
      break;
    }
  }
  expect(hitV).toBeGreaterThan(0);

  // The point mirrored about screen center is empty space.
  const mirrored = await engine.call<PickResult>("pick", { u: 0.5, v: 1 - hitV });
  expect(mirrored.hit === false || mirrored.id !== cube.id).toBe(true);
});

test("the engine logged no validation errors", async () => {
  await engine.settle(500);
  expect(engine.validationErrors()).toEqual([]);
});
