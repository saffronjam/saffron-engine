/// A tiny synchronous event bus that the resizable dock `Layout` pings whenever a
/// PanelGroup's layout settles (`onLayoutChanged`), so the `ViewportPanel` can fire
/// an exact resize-end commit for the reparented native window. This is decoupled
/// from the Zustand store on purpose: a panel-split layout change is a transient UI
/// signal, not editor state, so it should not churn the store or trigger renders.
///
/// The ViewportPanel's ResizeObserver already catches host-div geometry changes
/// during a drag. This bus covers non-resize layout events, including tab switches
/// that can disturb the native child window without changing the measured rect.
export interface LayoutSettledEvent {
  force?: boolean;
}

type LayoutListener = (event: LayoutSettledEvent) => void;

const listeners = new Set<LayoutListener>();

/// Subscribe to layout-settled notifications. Returns an unsubscribe fn.
export function onLayoutSettled(listener: LayoutListener): () => void {
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
}

/// Notify every subscriber that a dock layout just settled.
export function emitLayoutSettled(event: LayoutSettledEvent = {}): void {
  for (const listener of listeners) {
    listener(event);
  }
}
