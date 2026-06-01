/// The asset View modal: a large (512 px) square preview of a single catalog asset
/// (parity with `viewerPanel`, editor_panels.cpp:327-351, and the 512 re-render at
/// editor_app.cppm:165). A shadcn Dialog, opened by a tile double-click.
///
/// OCCLUSION (the core viewport-bridge constraint): the native SaffronEditor window
/// is reparented OVER the viewport div and ALWAYS paints on top of the webview — a
/// centered modal over the viewport rect would be hidden behind the native child.
/// So while the viewer is open we set `store.viewportHidden`, which the
/// ViewportPanel reads to park the native window off-screen; closing the viewer
/// clears the flag and the ViewportPanel re-glues the native window to its div. The
/// preview itself is a base64 PNG fetched over the socket (no native surface), so it
/// renders entirely in the webview and is never occluded.
import { useEffect, useState } from "react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import type { AssetEntry } from "../protocol";
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";

/// Preview render size (px), matching the C++ View re-render.
const VIEW_SIZE = 512;

function base64ToBlobUrl(b64: string): string {
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) {
    bytes[i] = bin.charCodeAt(i);
  }
  return URL.createObjectURL(new Blob([bytes], { type: "image/png" }));
}

export interface AssetViewerProps {
  /// The asset to preview, or null when the modal is closed.
  entry: AssetEntry | null;
  onClose(): void;
}

export function AssetViewer({ entry, onClose }: AssetViewerProps) {
  const [url, setUrl] = useState<string | null>(null);
  const setViewportHidden = useEditorStore((s) => s.setViewportHidden);
  const open = entry !== null;

  // Park the native viewport while the modal is open; always restore on close /
  // unmount so a stuck flag can never leave the viewport hidden.
  useEffect(() => {
    if (!open) {
      return;
    }
    setViewportHidden(true);
    return () => {
      setViewportHidden(false);
    };
  }, [open, setViewportHidden]);

  // Fetch the 512 preview when the asset changes; revoke the previous URL and the
  // final URL on close/unmount (no blob leak — this URL is NOT the shared cache).
  useEffect(() => {
    if (!entry) {
      setUrl(null);
      return;
    }
    let cancelled = false;
    let created: string | null = null;
    void client
      .viewAsset(entry.id, VIEW_SIZE)
      .then((thumb) => {
        if (cancelled) {
          return;
        }
        created = base64ToBlobUrl(thumb.base64);
        setUrl(created);
      })
      .catch(() => {
        // Leave `url` null; the dialog shows the empty preview frame.
      });
    return () => {
      cancelled = true;
      if (created) {
        URL.revokeObjectURL(created);
      }
      setUrl(null);
    };
  }, [entry]);

  return (
    <Dialog
      open={open}
      onOpenChange={(next) => {
        if (!next) {
          onClose();
        }
      }}
    >
      <DialogContent className="sm:max-w-[560px]">
        <DialogHeader>
          <DialogTitle className="truncate font-mono text-sm">
            {entry?.name ?? "Asset"}
          </DialogTitle>
        </DialogHeader>
        <div className="flex aspect-square w-full items-center justify-center overflow-hidden rounded-md border border-border bg-muted">
          {url ? (
            <img
              src={url}
              alt={entry?.name ?? "preview"}
              className="size-full object-contain"
              draggable={false}
            />
          ) : (
            <span className="text-xs italic text-muted-foreground">Rendering preview…</span>
          )}
        </div>
      </DialogContent>
    </Dialog>
  );
}
