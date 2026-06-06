/// The viewport region: a div the engine's render shows through. It never renders
/// pixels — it only owns the screen rectangle and forwards pointer input to the
/// engine over the control plane. A <LoadingOverlay/> sibling covers the region
/// while the renderer is not yet ready.
import { useCallback, useEffect, useLayoutEffect, useMemo, useRef } from "react";
import { client } from "../control/client";
import { makeCoalescer } from "../control/coalesce";
import { useEditorStore } from "../state/store";
import { LoadingOverlay } from "../app/LoadingOverlay";

/// Pointer travel (CSS px) below which a press-release is treated as a click
/// (ray-pick) rather than a gizmo drag.
const DRAG_THRESHOLD_PX = 3;

/// Throttle for streamed gizmo pointer phases (hover/drag), in milliseconds.
const GIZMO_STREAM_MS = 16;

/// Normalized [0,1] viewport coordinate, (0,0) = top-left.
interface Uv {
  u: number;
  v: number;
}

/// Map a pointer event to {u,v} in [0,1] using the panel's own client rect.
function eventToUv(el: HTMLElement, event: PointerEvent): Uv {
  const rect = el.getBoundingClientRect();
  if (rect.width <= 0 || rect.height <= 0) {
    return { u: 0, v: 0 };
  }
  const u = (event.clientX - rect.left) / rect.width;
  const v = (event.clientY - rect.top) / rect.height;
  return {
    u: Math.min(1, Math.max(0, u)),
    v: Math.min(1, Math.max(0, v)),
  };
}

export function ViewportPanel() {
  const hostRef = useRef<HTMLDivElement | null>(null);
  const attachedRef = useRef(false);
  const setPhase = useEditorStore((s) => s.setPhase);
  const setSelectedId = useEditorStore((s) => s.setSelectedId);
  const setDragActive = useEditorStore((s) => s.setDragActive);

  // Coalescers stream the hover and drag phases to the engine at >= GIZMO_STREAM_MS
  // apart, buffering only the latest NDC so a burst of pointermove collapses to one
  // in-flight call. Stable across renders.
  const hoverCoalescer = useMemo(
    () =>
      makeCoalescer<Uv>({
        throttleMs: GIZMO_STREAM_MS,
        send: ({ u, v }) => client.gizmoPointer("hover", u * 2 - 1, v * 2 - 1),
      }),
    [],
  );
  const dragCoalescer = useMemo(
    () =>
      makeCoalescer<Uv>({
        throttleMs: GIZMO_STREAM_MS,
        send: ({ u, v }) => client.gizmoPointer("drag", u * 2 - 1, v * 2 - 1),
      }),
    [],
  );

  // Optimistic post-pick selection: a hit sets store.selectedId immediately so the
  // UI does not wait a full reconcile interval. Empty space deselects.
  const runPick = useCallback(
    async ({ u, v }: Uv): Promise<void> => {
      try {
        const result = await client.pick(u, v);
        if (result.hit && result.id) {
          setSelectedId(result.id);
        } else {
          setSelectedId(null);
        }
      } catch {
        // The engine may be briefly busy; the reconcile poll recovers selection.
      }
    },
    [setSelectedId],
  );

  // Readiness: probe the control plane until the engine has booted + bound its
  // socket, then flip the phase. The `engine-phase` events are emitted from the Rust
  // `.setup()` hook BEFORE this webview registers its listener (Tauri does not buffer
  // pre-listen events), so the probe — not the event — is the gate. (`cancelled`
  // makes any pending retry a no-op after unmount.)
  useLayoutEffect(() => {
    let cancelled = false;

    const probe = async (): Promise<void> => {
      if (cancelled || attachedRef.current) {
        return;
      }
      try {
        await client.viewportNativeInfo();
      } catch {
        if (cancelled) {
          return;
        }
        setTimeout(() => void probe(), 150);
        return;
      }
      if (cancelled) {
        return;
      }
      attachedRef.current = true;
      setPhase("ready");
    };

    void probe();

    return () => {
      cancelled = true;
    };
  }, [setPhase]);

  // Pointer interaction: every press sends `begin`; if the pointer then travels
  // past DRAG_THRESHOLD_PX it is a gizmo drag (streamed `drag` + dragActive guard),
  // otherwise the release is a click that ray-picks. Release always sends `end`.
  // A bare move (no button down) streams `hover` so the engine highlights handles.
  useEffect(() => {
    const el = hostRef.current;
    if (!el) {
      return;
    }

    // Press-gesture state, reset on each pointerdown / pointerup.
    let pointerId: number | null = null;
    let startUv: Uv | null = null;
    let startClientX = 0;
    let startClientY = 0;
    let dragging = false;

    const ndc = (uv: Uv): { x: number; y: number } => ({
      x: uv.u * 2 - 1,
      y: uv.v * 2 - 1,
    });

    const onPointerDown = (event: PointerEvent): void => {
      // Left button only; the engine owns RMB-look / camera input.
      if (event.button !== 0 || pointerId !== null) {
        return;
      }
      pointerId = event.pointerId;
      startUv = eventToUv(el, event);
      startClientX = event.clientX;
      startClientY = event.clientY;
      dragging = false;
      el.setPointerCapture(event.pointerId);
      const { x, y } = ndc(startUv);
      void client.gizmoPointer("begin", x, y).catch(() => {});
    };

    const onPointerMove = (event: PointerEvent): void => {
      const uv = eventToUv(el, event);
      if (pointerId === null) {
        // Hovering (no button down): keep the engine's handle highlight fresh.
        hoverCoalescer.push(uv);
        return;
      }
      if (event.pointerId !== pointerId) {
        return;
      }
      if (!dragging) {
        const moved =
          Math.abs(event.clientX - startClientX) > DRAG_THRESHOLD_PX ||
          Math.abs(event.clientY - startClientY) > DRAG_THRESHOLD_PX;
        if (!moved) {
          return;
        }
        dragging = true;
        setDragActive(true);
      }
      dragCoalescer.push(uv);
    };

    const finishPress = (event: PointerEvent): void => {
      if (pointerId === null || event.pointerId !== pointerId) {
        return;
      }
      const uv = eventToUv(el, event);
      const { x, y } = ndc(uv);
      void client.gizmoPointer("end", x, y).catch(() => {});
      if (el.hasPointerCapture(event.pointerId)) {
        el.releasePointerCapture(event.pointerId);
      }
      const wasDragging = dragging;
      const downUv = startUv;
      pointerId = null;
      startUv = null;
      dragging = false;
      if (wasDragging) {
        // The authoritative transform is committed engine-side on `end`; let the
        // poll resume and reconcile it.
        setDragActive(false);
      } else if (downUv) {
        // No drag: a plain left-click → ray-pick at the press location.
        void runPick(downUv);
      }
    };

    el.addEventListener("pointerdown", onPointerDown);
    el.addEventListener("pointermove", onPointerMove);
    el.addEventListener("pointerup", finishPress);
    el.addEventListener("pointercancel", finishPress);

    return () => {
      el.removeEventListener("pointerdown", onPointerDown);
      el.removeEventListener("pointermove", onPointerMove);
      el.removeEventListener("pointerup", finishPress);
      el.removeEventListener("pointercancel", finishPress);
      if (pointerId !== null && el.hasPointerCapture(pointerId)) {
        el.releasePointerCapture(pointerId);
      }
      // Drop any drag guard the gesture left set.
      if (dragging) {
        setDragActive(false);
      }
    };
  }, [hoverCoalescer, dragCoalescer, runPick, setDragActive]);

  // h-full/w-full (not flex-1): the panel's content div is block-level, so flex-1
  // would be inert here — the viewport must fill its panel rect by explicit size.
  return (
    <div className="relative h-full w-full overflow-hidden bg-black">
      <div ref={hostRef} className="viewport-host" />
      <LoadingOverlay />
    </div>
  );
}
