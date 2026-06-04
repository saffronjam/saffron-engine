/// Write-coalescer for high-frequency mutations (gizmo echo, scrub fields, sliders).
/// Buffers the latest pushed value, throttles sends to >= throttleMs apart, and
/// tracks sent/completed/in-flight counters around the async send. Ported and
/// generalized from the worktree `queueTransform`.

export interface CoalescerStats {
  sent: number;
  completed: number;
  inFlight: number;
}

export interface Coalescer<T> {
  /// Record the latest value; sends it now if the throttle window has elapsed,
  /// otherwise it is buffered and overwrites any prior pending value.
  push(value: T): void;
  /// Counters for diagnostics (sent/completed since process start, current in-flight).
  stats(): CoalescerStats;
}

export interface CoalescerOptions<T> {
  /// Minimum milliseconds between two sends (default 4).
  throttleMs?: number;
  /// The async sink for the latest buffered value.
  send: (latest: T) => Promise<unknown>;
}

/// Minimum gap between two logged coalesced-write rejections, so a sustained
/// failure during a scrub stream produces at most one console.error per window
/// instead of one per dropped frame.
const ERROR_LOG_THROTTLE_MS = 2000;

export function makeCoalescer<T>(options: CoalescerOptions<T>): Coalescer<T> {
  const throttleMs = options.throttleMs ?? 4;
  const { send } = options;

  let pending: { value: T } | null = null;
  let lastSentAt = 0;
  let sent = 0;
  let completed = 0;
  let inFlight = 0;
  let timer: ReturnType<typeof setTimeout> | null = null;
  let lastErrorLoggedAt = 0;

  // A coalesced write is the tail of a scrub/drag stream, so a per-rejection toast
  // would spam. Drop the value (the next push supersedes it) but log at most once
  // per ERROR_LOG_THROTTLE_MS so a persistent failure is not fully silent.
  function logRejection(err: unknown): void {
    const now = performance.now();
    if (now - lastErrorLoggedAt < ERROR_LOG_THROTTLE_MS) {
      return;
    }
    lastErrorLoggedAt = now;
    // eslint-disable-next-line no-console
    console.error("coalesced write rejected:", err);
  }

  function flush(): void {
    const buffered = pending;
    pending = null;
    if (!buffered) {
      return;
    }
    lastSentAt = performance.now();
    sent += 1;
    inFlight += 1;
    void Promise.resolve(send(buffered.value))
      .catch(logRejection)
      .finally(() => {
        completed += 1;
        inFlight = Math.max(0, inFlight - 1);
      });
  }

  return {
    push(value: T): void {
      pending = { value };
      const now = performance.now();
      const elapsed = now - lastSentAt;
      if (elapsed >= throttleMs) {
        if (timer !== null) {
          clearTimeout(timer);
          timer = null;
        }
        flush();
        return;
      }
      // Within the throttle window: schedule a trailing flush so the latest value
      // is never dropped, coalescing any further pushes into it.
      if (timer === null) {
        timer = setTimeout(() => {
          timer = null;
          flush();
        }, throttleMs - elapsed);
      }
    },
    stats(): CoalescerStats {
      return { sent, completed, inFlight };
    },
  };
}
