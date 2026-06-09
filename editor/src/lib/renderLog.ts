/// Dev-mode render-frequency logger. Components call `logRender("Name")` at the
/// top of their render; while dev mode is on (the titlebar chip), aggregated
/// per-component render counts are flushed to the console once per second:
///
///   [renders/s] AssetTile×412  AssetPanelBody×103  AssetsPanel×103 …
///
/// Reads the store via getState() so the logger never subscribes (it must not
/// add re-renders of its own), and is a no-op while dev mode is off. Counts
/// include renders React later discards (StrictMode double-renders in dev), so
/// treat the numbers as relative, not exact.
import { useEditorStore } from "../state/store";

const counts = new Map<string, number>();
let flushTimer: number | null = null;

export function logRender(name: string): void {
  if (!useEditorStore.getState().devMode) {
    return;
  }
  counts.set(name, (counts.get(name) ?? 0) + 1);
  if (flushTimer !== null) {
    return;
  }
  flushTimer = window.setTimeout(() => {
    flushTimer = null;
    const rows = [...counts.entries()].sort((a, b) => b[1] - a[1]);
    counts.clear();
    console.info(`[renders/s] ${rows.map(([name_, count]) => `${name_}×${count}`).join("  ")}`);
  }, 1000);
}
