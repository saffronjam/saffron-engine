import { describe, expect, test } from "bun:test";
import { niceCeil } from "./FrameTimeGraph";

// niceCeil rounds an axis value up to the nearest 1 / 2 / 5 × 10^k so the Y axis snaps to
// stable, readable maxima. These tests pin its real behaviour (the source is the truth) — they
// do NOT exercise the sticky/shrink-dwell ceiling, which lives inside the React component's
// yRange closure and is not exportable (see SKIPPED note in the task report).

describe("niceCeil — non-positive inputs floor to 1", () => {
  test("zero returns 1", () => {
    expect(niceCeil(0)).toBe(1);
  });

  test("negative values return 1", () => {
    expect(niceCeil(-0.5)).toBe(1);
    expect(niceCeil(-42)).toBe(1);
    expect(niceCeil(-1e9)).toBe(1);
  });

  test("negative zero returns 1", () => {
    expect(niceCeil(-0)).toBe(1);
  });
});

describe("niceCeil — snaps to 1/2/5 within a decade", () => {
  // base = 1 (decade [1, 10]).
  test("exact 1/2/5 levels round to themselves", () => {
    expect(niceCeil(1)).toBe(1);
    expect(niceCeil(2)).toBe(2);
    expect(niceCeil(5)).toBe(5);
    expect(niceCeil(10)).toBe(10);
  });

  test("values between levels round up to the next clean level", () => {
    expect(niceCeil(1.5)).toBe(2); // (1, 2] -> 2
    expect(niceCeil(2.1)).toBe(5); // (2, 5] -> 5
    expect(niceCeil(3)).toBe(5);
    expect(niceCeil(4.9)).toBe(5);
    expect(niceCeil(5.1)).toBe(10); // (5, 10] -> 10
    expect(niceCeil(7)).toBe(10);
    expect(niceCeil(9.9)).toBe(10);
  });

  test("a hair above 1 rounds to 2 (the first level it does not fit under)", () => {
    expect(niceCeil(1.0001)).toBe(2);
  });
});

describe("niceCeil — higher decades scale by 10^k", () => {
  // base = 10 (decade [10, 100]).
  test("decade [10, 100]", () => {
    expect(niceCeil(11)).toBe(20);
    expect(niceCeil(16)).toBe(20);
    expect(niceCeil(20)).toBe(20);
    expect(niceCeil(21)).toBe(50);
    expect(niceCeil(50)).toBe(50);
    expect(niceCeil(51)).toBe(100);
    expect(niceCeil(99)).toBe(100);
    expect(niceCeil(100)).toBe(100);
  });

  // base = 100 (decade [100, 1000]).
  test("decade [100, 1000]", () => {
    expect(niceCeil(101)).toBe(200);
    expect(niceCeil(250)).toBe(500);
    expect(niceCeil(500)).toBe(500);
    expect(niceCeil(501)).toBe(1000);
    expect(niceCeil(1000)).toBe(1000);
  });

  test("a large value lands on a clean power-scaled level", () => {
    expect(niceCeil(123456)).toBe(200000);
  });
});

describe("niceCeil — sub-unit (fractional) decades", () => {
  // base = 0.1 (decade [0.1, 1]).
  test("decade [0.1, 1]", () => {
    expect(niceCeil(0.1)).toBeCloseTo(0.1);
    expect(niceCeil(0.15)).toBeCloseTo(0.2);
    expect(niceCeil(0.2)).toBeCloseTo(0.2);
    expect(niceCeil(0.5)).toBeCloseTo(0.5);
    expect(niceCeil(0.6)).toBeCloseTo(1);
  });

  // base = 0.01 (decade [0.01, 0.1]) — a plausible sub-millisecond frame budget scale.
  test("decade [0.01, 0.1]", () => {
    expect(niceCeil(0.011)).toBeCloseTo(0.02);
    expect(niceCeil(0.05)).toBeCloseTo(0.05);
    expect(niceCeil(0.099)).toBeCloseTo(0.1);
  });
});

describe("niceCeil — frame-time-flavoured values (ms)", () => {
  // The graph feeds it data-max * HEADROOM and budget * BUDGET_ANCHOR; these are realistic ms.
  test("16.7ms (60fps budget) area rounds up to 20", () => {
    expect(niceCeil(16.7)).toBe(20);
  });

  test("33.3ms (30fps) rounds up to 50", () => {
    expect(niceCeil(33.3)).toBe(50);
  });

  test("8.3ms (120fps) rounds up to 10", () => {
    expect(niceCeil(8.3)).toBe(10);
  });
});

describe("niceCeil — never returns below the input", () => {
  const samples = [
    0, 0.001, 0.01, 0.05, 0.099, 0.1, 0.5, 0.999, 1, 1.0001, 1.5, 2, 2.1, 5, 7, 9.9, 10, 11, 16.7,
    33.3, 50, 99, 100, 250, 500, 999, 1000, 123456,
  ];

  test("every sample maps to a ceiling >= itself", () => {
    for (const v of samples) {
      expect(niceCeil(v)).toBeGreaterThanOrEqual(v);
    }
  });

  test("negative inputs are also bounded below by their (negative) value", () => {
    for (const v of [-1, -100, -0.5]) {
      expect(niceCeil(v)).toBeGreaterThanOrEqual(v);
    }
  });
});

describe("niceCeil — always a 1/2/5 × 10^k mantissa", () => {
  // The returned value's mantissa (value / 10^floor(log10(value))) is one of 1, 2, 5 — or it is
  // 10 collapsed into the next decade's 1. Sampling confirms the snap is to a clean level.
  const samples = [0.011, 0.13, 0.6, 1.5, 3, 7, 16, 21, 51, 250, 7777];

  test("each ceiling's leading digits are a clean 1/2/5 step", () => {
    for (const v of samples) {
      const r = niceCeil(v);
      const mantissa = r / Math.pow(10, Math.floor(Math.log10(r)));
      // Allow tiny float drift before comparing the mantissa to the clean set.
      const rounded = Math.round(mantissa * 1e6) / 1e6;
      expect([1, 2, 5]).toContain(rounded);
    }
  });
});
