/// One asset catalog tile: a lazy thumbnail (mesh = its 3D render, texture = its
/// image, else a lucide type icon), an in-place rename input, and an HTML5 drag
/// SOURCE carrying `application/x-se-asset` (the React analog of the C++ `SE_ASSET`
/// / `AssetDragPayload`). Double-click opens the View modal. Parity target:
/// `assetCatalogPanel` (editor_panels.cpp:226-325) + the `thumbnailFor` fallbacks.
import { useEffect, useRef, useState } from "react";
import { Box, File, Image as ImageIcon } from "lucide-react";
import { client } from "../control/client";
import { getThumbnailUrl, useEditorStore } from "../state/store";
import type { AssetEntry } from "../protocol";
import { cn } from "@/lib/utils";
import { Input } from "@/components/ui/input";

/// The DnD payload written to `application/x-se-asset` (distinct from an OS file
/// drop). `type` lets a drop target type-gate the accept (parity with the C++
/// `drag->type == type` guard, editor_components.cpp:77).
export const ASSET_DND_MIME = "application/x-se-asset";

export interface AssetDragPayload {
  id: string;
  type: AssetEntry["type"];
}

/// Try to read an asset drag payload off a DataTransfer; null if it is not one.
export function readAssetPayload(dt: DataTransfer): AssetDragPayload | null {
  const raw = dt.getData(ASSET_DND_MIME);
  if (!raw) {
    return null;
  }
  try {
    const parsed = JSON.parse(raw) as Partial<AssetDragPayload>;
    if (typeof parsed.id === "string" && typeof parsed.type === "string") {
      return { id: parsed.id, type: parsed.type };
    }
  } catch {
    // Malformed payload; treat as not-an-asset.
  }
  return null;
}

function TypeIcon({ type }: { type: AssetEntry["type"] }) {
  const className = "size-7 text-muted-foreground";
  if (type === "mesh") {
    return <Box className={className} />;
  }
  if (type === "texture") {
    return <ImageIcon className={className} />;
  }
  return <File className={className} />;
}

export interface AssetTileProps {
  entry: AssetEntry;
  onView(entry: AssetEntry): void;
}

/// Render the grid thumbnail at 128 px (parity with editor_app.cppm:138) and
/// display it at the 72-px tile size (parity with `tileSize`).
const THUMBNAIL_FETCH_SIZE = 128;

export function AssetTile({ entry, onView }: AssetTileProps) {
  const [url, setUrl] = useState<string | null>(null);
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState(entry.name);
  const refreshAssets = useEditorStore((s) => s.refreshAssets);

  // Lazy thumbnail: fetch on mount / id change. The shared cache dedupes across
  // tiles; a rejection leaves `url` null so the type icon shows (parity with the
  // `*Icon.id` fallback in thumbnailFor).
  useEffect(() => {
    let cancelled = false;
    setUrl(null);
    void getThumbnailUrl(entry.id, THUMBNAIL_FETCH_SIZE)
      .then((resolved) => {
        if (!cancelled) {
          setUrl(resolved);
        }
      })
      .catch(() => {
        // Keep the type-icon fallback.
      });
    return () => {
      cancelled = true;
    };
  }, [entry.id]);

  // Keep the rename draft in sync when the catalog name changes externally
  // (e.g. an `se rename-asset` reflected by the poll), but not while editing.
  useEffect(() => {
    if (!editing) {
      setDraft(entry.name);
    }
  }, [entry.name, editing]);

  const commitRename = (): void => {
    setEditing(false);
    const next = draft.trim();
    if (next.length === 0 || next === entry.name) {
      setDraft(entry.name);
      return;
    }
    void client
      .renameAsset(entry.id, next)
      .then(() => refreshAssets())
      .catch(() => setDraft(entry.name));
  };

  const onDragStart = (event: React.DragEvent<HTMLDivElement>): void => {
    const payload: AssetDragPayload = { id: entry.id, type: entry.type };
    event.dataTransfer.setData(ASSET_DND_MIME, JSON.stringify(payload));
    event.dataTransfer.effectAllowed = "copy";
  };

  return (
    <div
      // Editing disables the drag so a tile-drag never starts while typing.
      draggable={!editing}
      onDragStart={onDragStart}
      onDoubleClick={() => onView(entry)}
      title={`${entry.name}\n${entry.path}`}
      className={cn(
        "group flex w-[72px] cursor-grab flex-col gap-1 rounded-md border border-border bg-background p-1",
        "transition-colors hover:border-ring hover:bg-accent/40 active:cursor-grabbing",
      )}
    >
      <div className="flex aspect-square w-full items-center justify-center overflow-hidden rounded-sm bg-muted">
        {url ? (
          <img
            src={url}
            alt={entry.name}
            className="size-full object-contain"
            draggable={false}
          />
        ) : (
          <TypeIcon type={entry.type} />
        )}
      </div>
      {editing ? (
        <RenameInput
          value={draft}
          onChange={setDraft}
          onCommit={commitRename}
          onCancel={() => {
            setEditing(false);
            setDraft(entry.name);
          }}
        />
      ) : (
        <button
          type="button"
          className="truncate rounded-sm px-0.5 text-center text-[10px] leading-tight text-foreground hover:bg-accent"
          title={entry.name}
          onDoubleClick={(event) => {
            // Don't bubble to the tile's View handler — a name double-click renames.
            event.stopPropagation();
            setDraft(entry.name);
            setEditing(true);
          }}
        >
          {entry.name}
        </button>
      )}
    </div>
  );
}

interface RenameInputProps {
  value: string;
  onChange(value: string): void;
  onCommit(): void;
  onCancel(): void;
}

function RenameInput({ value, onChange, onCommit, onCancel }: RenameInputProps) {
  const ref = useRef<HTMLInputElement | null>(null);
  useEffect(() => {
    ref.current?.focus();
    ref.current?.select();
  }, []);
  return (
    <Input
      ref={ref}
      value={value}
      // Stop pointer/drag propagation so the tile drag never starts mid-edit.
      onPointerDown={(event) => event.stopPropagation()}
      onChange={(event) => onChange(event.currentTarget.value)}
      onBlur={onCommit}
      onKeyDown={(event) => {
        if (event.key === "Enter") {
          event.preventDefault();
          onCommit();
        } else if (event.key === "Escape") {
          event.preventDefault();
          onCancel();
        }
      }}
      className="h-5 rounded-sm px-1 py-0 text-center font-mono text-[10px]"
    />
  );
}
