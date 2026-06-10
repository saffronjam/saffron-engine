// Skeleton overlay + show-bones: the native line-skeleton overlay draws bone segments + joint
// dots over the selected rig, on top, in Edit and Play. This drives the set/get-skeleton-overlay
// control toggle (round-trip), proves the overlay actually renders (selecting a rig and turning
// bones on changes pixels), and asserts the run stays validation-clean.

import { afterAll, beforeAll, expect, test } from "bun:test";
import { existsSync, readFileSync, rmSync } from "node:fs";
import { join } from "node:path";
import { Engine, REPO } from "./harness.ts";

let engine: Engine;
let meshId = "";
const FIXTURE = join(REPO, "engine", "assets", "models", "animated-strip.gltf");
const shots: string[] = [];

interface OverlayState {
  show: boolean;
  axes: boolean;
  jointSize: number;
}

beforeAll(async () => {
  engine = await Engine.boot({ SAFFRON_AUTO_EMPTY_PROJECT: "1" });
  await engine.call("set-camera", { yaw: 0, pitch: 0 });
  const imported = await engine.call<{ id: string; name: string }>("import-model", { path: FIXTURE });
  meshId = imported.id;
  await engine.call("select", { entity: meshId });
  await engine.call("focus", { entity: meshId });
  await engine.settle();
});
afterAll(async () => {
  await engine?.shutdown();
  for (const shot of shots) {
    rmSync(shot, { force: true });
  }
});

async function screenshot(tag: string): Promise<Buffer> {
  const path = `/tmp/saffron-e2e-skel-${process.pid}-${tag}.png`;
  shots.push(path);
  await engine.call("screenshot", { target: "viewport", path });
  const deadline = Date.now() + 10_000;
  while (!existsSync(path)) {
    if (Date.now() > deadline) {
      throw new Error(`screenshot ${tag} never landed at ${path}`);
    }
    await engine.settle(100);
  }
  await engine.settle(200);
  return readFileSync(path);
}

test("the overlay is off by default", async () => {
  const state = await engine.call<OverlayState>("get-skeleton-overlay", {});
  expect(state.show).toBe(false);
});

test("set-skeleton-overlay round-trips through get", async () => {
  const set = await engine.call<OverlayState>("set-skeleton-overlay", { show: true, axes: true, jointSize: 6 });
  expect(set.show).toBe(true);
  expect(set.axes).toBe(true);
  expect(set.jointSize).toBeCloseTo(6, 4);
  const got = await engine.call<OverlayState>("get-skeleton-overlay", {});
  expect(got.show).toBe(true);
  expect(got.axes).toBe(true);
});

test("turning bones on draws the skeleton over the selected rig", async () => {
  await engine.call("set-skeleton-overlay", { show: false });
  await engine.settle(300);
  const off = await screenshot("off");
  await engine.call("set-skeleton-overlay", { show: true, axes: true });
  await engine.settle(300);
  const on = await screenshot("on");
  expect(on.equals(off)).toBe(false);
});

test("the engine logged no validation errors", () => {
  expect(engine.validationErrors()).toEqual([]);
});
