import { describe, expect, test } from "bun:test";
import { chooseTickStepMs, formatTick, withAlpha } from "./timelineCanvas";

// Pure ruler/transform/color helpers from the TimelineCanvas renderer. The
// instance methods (secToX/xToSec) and the draw routines need a real 2D canvas
// context, so they are not exercised here (see the subagent's uncertainties).

describe("chooseTickStepMs", () => {
  // The candidate steps (ms), kept in sync with TICK_STEPS_MS in the source.
  const STEPS = [10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 30000, 60000];
  const TARGET_TICK_PX = 64;

  test("returns a step from the candidate list", () => {
    expect(STEPS).toContain(chooseTickStepMs(1, 640));
    expect(STEPS).toContain(chooseTickStepMs(10, 1280));
  });

  test("a 1s ruler 640px wide picks the 100ms step", () => {
    // pxPerMs = 640 / 1000 = 0.64; smallest step with step*0.64 >= 64 is 100.
    expect(chooseTickStepMs(1, 640)).toBe(100);
  });

  test("a 10s ruler 640px wide picks the 1000ms (1s) step", () => {
    // pxPerMs = 640 / 10000 = 0.064; smallest step with step*0.064 >= 64 is 1000.
    expect(chooseTickStepMs(10, 640)).toBe(1000);
  });

  test("a narrow 1s ruler (64px) needs the 1000ms step", () => {
    // pxPerMs = 64 / 1000 = 0.064; only step >= 1000 clears the 64px target.
    expect(chooseTickStepMs(1, 64)).toBe(1000);
  });

  test("the >= comparison is inclusive at the exact target spacing", () => {
    // pxPerMs = 1280 / 1000 = 1.28; 50 * 1.28 === 64 exactly, so 50 qualifies
    // while 25 (=> 32px) does not.
    expect(chooseTickStepMs(1, 1280)).toBe(50);
  });

  test("a wide ruler can pick the finest 10ms step", () => {
    // pxPerMs large enough that even 10ms clears the target.
    // 10 * pxPerMs >= 64 => pxPerMs >= 6.4; duration 1s => width >= 6400.
    expect(chooseTickStepMs(1, 6400)).toBe(10);
  });

  test("falls back to the coarsest step when nothing clears the target", () => {
    // A huge duration in a 1px ruler: no candidate step reaches 64px spacing,
    // so the function returns the last (coarsest) candidate.
    expect(chooseTickStepMs(1000, 1)).toBe(60000);
    expect(chooseTickStepMs(1_000_000, 100)).toBe(60000);
  });

  test("guards a zero/negative duration via the Math.max(_, 1) floor", () => {
    // durationSec*1000 floored to 1 => pxPerMs = width; with width 640 even the
    // finest 10ms step clears the target (10 * 640 >> 64).
    expect(chooseTickStepMs(0, 640)).toBe(10);
    expect(chooseTickStepMs(-5, 640)).toBe(10);
  });

  test("never returns a step below the target spacing unless it is the fallback", () => {
    // For a range of inputs that DO have a qualifying step, the chosen step must
    // actually clear the target with the computed pxPerMs.
    for (const [dur, width] of [
      [1, 640],
      [2, 800],
      [5, 1000],
      [10, 1280],
      [0.5, 600],
    ] as const) {
      const step = chooseTickStepMs(dur, width);
      const pxPerMs = width / Math.max(dur * 1000, 1);
      expect(step * pxPerMs).toBeGreaterThanOrEqual(TARGET_TICK_PX);
    }
  });
});

describe("formatTick", () => {
  test("zero is rendered as 0ms", () => {
    expect(formatTick(0)).toBe("0ms");
  });

  test("whole-second multiples render in seconds", () => {
    expect(formatTick(1000)).toBe("1s");
    expect(formatTick(5000)).toBe("5s");
    expect(formatTick(60000)).toBe("60s");
  });

  test("sub-second values render in milliseconds", () => {
    expect(formatTick(10)).toBe("10ms");
    expect(formatTick(250)).toBe("250ms");
    expect(formatTick(500)).toBe("500ms");
  });

  test("non-whole-second values stay in milliseconds even above 1000", () => {
    // 2500 % 1000 === 500 != 0, so it is not a whole-second multiple.
    expect(formatTick(2500)).toBe("2500ms");
    expect(formatTick(1500)).toBe("1500ms");
  });

  test("fractional milliseconds are rounded", () => {
    expect(formatTick(1500.4)).toBe("1500ms");
    expect(formatTick(1500.6)).toBe("1501ms");
  });
});

describe("withAlpha", () => {
  test("expands a 6-digit hex to rgba with the given alpha", () => {
    expect(withAlpha("#ff8800", 0.32)).toBe("rgba(255, 136, 0, 0.32)");
  });

  test("expands a 3-digit shorthand hex by doubling each nibble", () => {
    expect(withAlpha("#abc", 0.5)).toBe("rgba(170, 187, 204, 0.5)");
    expect(withAlpha("#fff", 1)).toBe("rgba(255, 255, 255, 1)");
    expect(withAlpha("#000", 0)).toBe("rgba(0, 0, 0, 0)");
  });

  test("trims surrounding whitespace before parsing", () => {
    expect(withAlpha("  #00ff00  ", 0.25)).toBe("rgba(0, 255, 0, 0.25)");
  });

  test("returns a non-hex color string unchanged", () => {
    expect(withAlpha("rgb(1, 2, 3)", 0.4)).toBe("rgb(1, 2, 3)");
    expect(withAlpha("rgba(1, 2, 3, 0.9)", 0.4)).toBe("rgba(1, 2, 3, 0.9)");
    expect(withAlpha("red", 0.4)).toBe("red");
  });

  test("a lowercase or uppercase hex parses identically", () => {
    expect(withAlpha("#FF8800", 0.32)).toBe("rgba(255, 136, 0, 0.32)");
  });
});
