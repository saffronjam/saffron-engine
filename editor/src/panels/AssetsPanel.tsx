/// The Assets panel: the React port of the C++ `assetCatalogPanel`
/// (editor_panels.cpp:226-325). A responsive tile grid over `store.assets`, an
/// Import button (Tauri file dialog), an OS file-drop target, and the View modal.
/// Imports route by extension (parity with `importToCatalog`,
/// editor_app.cppm:188-205): images → import-texture (no spawn), everything else →
/// import-model (spawns + selects an entity, matching `se import-model`).
import { useCallback, useEffect, useRef, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { getCurrentWebview } from "@tauri-apps/api/webview";
import { Folder, FolderPlus, Pen, Plus, Trash } from "lucide-react";
import { client } from "../control/client";
import { invalidateThumbnails, useEditorStore, withNativeDialog } from "../state/store";
import { AssetTile, DeleteConfirm } from "../components/AssetTile";
import { ASSET_DND_MIME, readAssetPayload } from "../components/AssetTile";
import type { AssetEntry } from "../protocol";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { ScrollArea } from "@/components/ui/scroll-area";
import {
  ContextMenu,
  ContextMenuContent,
  ContextMenuItem,
  ContextMenuSeparator,
  ContextMenuTrigger,
} from "@/components/ui/context-menu";

/// Image extensions that import as a catalog texture; everything else is imported
/// as a model (parity with editor_app.cppm:201).
const TEXTURE_EXTS = new Set(["png", "jpg", "jpeg", "hdr", "tga", "bmp"]);

/// Model + image extensions offered in the file dialog.
const MODEL_EXTS = ["gltf", "glb", "obj", "smesh"];
const IMAGE_EXTS = ["png", "jpg", "jpeg", "hdr", "tga", "bmp"];

function extensionOf(path: string): string {
  const dot = path.lastIndexOf(".");
  return dot >= 0 ? path.slice(dot + 1).toLowerCase() : "";
}

async function importPath(path: string, folder: string | null): Promise<void> {
  try {
    if (TEXTURE_EXTS.has(extensionOf(path))) {
      const imported = await client.importTexture(path);
      if (folder) {
        await client.moveAsset(imported.texture, folder);
      }
    } else {
      const imported = await client.importModel(path);
      if (folder) {
        await moveImportedAsset(imported.mesh, folder);
        await moveImportedAsset(imported.albedoTexture, folder);
      }
    }
  } catch {
    // The engine reports a bad import as ok:false (a rejected promise); swallow so
    // a single bad file doesn't break a multi-file drop.
  }
}

export function AssetsPanel() {
  const assets = useEditorStore((s) => s.assets);
  const folders = useEditorStore((s) => s.assetFolders);
  const refreshAssets = useEditorStore((s) => s.refreshAssets);
  const nativeDialogOpen = useEditorStore((s) => s.nativeDialogOpen);
  const openAssetTab = useEditorStore((s) => s.openAssetTab);
  const closeViewTab = useEditorStore((s) => s.closeViewTab);
  const [currentFolder, setCurrentFolder] = useState<string | null>(null);
  const [creatingFolder, setCreatingFolder] = useState(false);
  const [renamingFolder, setRenamingFolder] = useState<string | null>(null);
  const [pendingAssetDelete, setPendingAssetDelete] = useState<PendingAssetDelete | null>(null);
  const [pendingFolderDelete, setPendingFolderDelete] = useState<string | null>(null);
  const [folderError, setFolderError] = useState<string | null>(null);
  const [dropActive, setDropActive] = useState(false);
  const visibleAssets = assets.filter((asset) => (asset.folder ?? "") === (currentFolder ?? ""));
  const rootAssets = assets.filter((asset) => !asset.folder);

  useEffect(() => {
    if (currentFolder && !folders.includes(currentFolder)) {
      setCurrentFolder(null);
    }
  }, [currentFolder, folders]);

  const importMany = useCallback(
    async (paths: string[]): Promise<void> => {
      for (const path of paths) {
        await importPath(path, currentFolder);
      }
      await refreshAssets();
    },
    [currentFolder, refreshAssets],
  );

  // OS file-drop: Tauri delivers native file drops via the webview drag-drop event
  // (a distinct channel from the HTML5 `application/x-se-asset` tile DnD). We only
  // import when the drop position is inside this panel's rect, so dropping a model
  // on the viewport doesn't trigger a catalog import here.
  useEffect(() => {
    let unlisten: (() => void) | null = null;
    let disposed = false;
    void getCurrentWebview()
      .onDragDropEvent((event) => {
        const payload = event.payload;
        if (payload.type === "enter" || payload.type === "over") {
          setDropActive(isInsidePanel(payload.position));
        } else if (payload.type === "leave") {
          setDropActive(false);
        } else if (payload.type === "drop") {
          setDropActive(false);
          if (payload.paths.length > 0 && isInsidePanel(payload.position)) {
            void importMany(payload.paths);
          }
        }
      })
      .then((fn) => {
        if (disposed) {
          fn();
        } else {
          unlisten = fn;
        }
      });
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, [importMany]);

  const onImportClick = useCallback(async (): Promise<void> => {
    const selection = await withNativeDialog(() =>
      open({
        multiple: true,
        filters: [
          { name: "Models & Images", extensions: [...MODEL_EXTS, ...IMAGE_EXTS] },
          { name: "Models", extensions: MODEL_EXTS },
          { name: "Images", extensions: IMAGE_EXTS },
        ],
      }),
    );
    if (!selection) {
      return;
    }
    await importMany(Array.isArray(selection) ? selection : [selection]);
  }, [importMany]);

  const onNewFolder = useCallback((): void => {
    setFolderError(null);
    setCreatingFolder(true);
  }, []);

  const commitNewFolder = useCallback(
    async (name: string): Promise<void> => {
      const folder = name.trim();
      if (!folder) {
        setCreatingFolder(false);
        setFolderError(null);
        return;
      }
      if (folders.includes(folder)) {
        setFolderError("Folder already exists");
        setCreatingFolder(true);
        return;
      }
      try {
        await client.createAssetFolder(folder);
        await refreshAssets();
        setCreatingFolder(false);
        setFolderError(null);
      } catch {
        setFolderError("Could not create folder");
        setCreatingFolder(true);
      }
    },
    [folders, refreshAssets],
  );

  const cancelNewFolder = useCallback((): void => {
    setCreatingFolder(false);
    setFolderError(null);
  }, []);

  const commitRenameFolder = useCallback(
    async (folder: string, name: string): Promise<void> => {
      const next = name.trim();
      if (!next || next === folder) {
        setRenamingFolder(null);
        setFolderError(null);
        return;
      }
      if (folders.includes(next)) {
        setFolderError("Folder already exists");
        setRenamingFolder(folder);
        return;
      }
      try {
        await client.renameAssetFolder(folder, next);
        await refreshAssets();
        if (currentFolder === folder) {
          setCurrentFolder(next);
        }
        setRenamingFolder(null);
        setFolderError(null);
      } catch {
        setFolderError("Could not rename folder");
        setRenamingFolder(folder);
      }
    },
    [currentFolder, folders, refreshAssets],
  );

  const cancelRenameFolder = useCallback((): void => {
    setRenamingFolder(null);
    setFolderError(null);
  }, []);

  const moveAssetToFolder = useCallback(
    async (assetId: string, folder: string | null): Promise<void> => {
      await client.moveAsset(assetId, folder);
      await refreshAssets();
    },
    [refreshAssets],
  );

  const requestDeleteAsset = useCallback(async (asset: AssetEntry): Promise<void> => {
    setPendingFolderDelete(null);
    const usages = await client.assetUsages(asset.id);
    const usageLines = usages.usages.map((usage) => {
      if (usage.entityName) {
        return `${usage.entityName}: ${usage.slot}`;
      }
      return usage.slot;
    });
    setPendingAssetDelete({
      asset,
      body:
        usageLines.length > 0
          ? `Clears ${usageLines.length} usage${usageLines.length === 1 ? "" : "s"}: ${usageLines.join(", ")}.`
          : "Removes the catalog entry and imported file.",
    });
  }, []);

  const confirmDeleteAsset = useCallback(
    async (asset: AssetEntry): Promise<void> => {
      await client.deleteAsset(asset.id);
      setPendingAssetDelete(null);
      closeViewTab(`asset:${asset.id}`);
      invalidateThumbnails();
      await refreshAssets();
    },
    [closeViewTab, refreshAssets],
  );

  const requestDeleteFolder = useCallback((folder: string): void => {
    setPendingAssetDelete(null);
    setPendingFolderDelete(folder);
  }, []);

  const confirmDeleteFolder = useCallback(
    async (folder: string): Promise<void> => {
      await client.deleteAssetFolder(folder);
      if (currentFolder === folder) {
        setCurrentFolder(null);
      }
      setPendingFolderDelete(null);
      await refreshAssets();
    },
    [currentFolder, refreshAssets],
  );

  return (
    <div className="flex h-full min-h-0 flex-col" data-asset-panel="true">
      <div className="flex h-10 flex-none items-center justify-between border-b border-border px-3">
        <span className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">
          Assets
        </span>
        <div className="flex items-center gap-2">
          <Button
            type="button"
            size="sm"
            variant="outline"
            className="gap-1"
            onClick={onNewFolder}
            title="New folder"
          >
            <FolderPlus />
            New Folder
          </Button>
          <Button
            type="button"
            size="sm"
            variant="outline"
            className="gap-1"
            onClick={() => void onImportClick()}
            disabled={nativeDialogOpen}
            title="Import a model or texture"
          >
            <Plus />
            Import
          </Button>
        </div>
      </div>
      <ContextMenu>
        <ContextMenuTrigger asChild>
          <div className="min-h-0 flex-1">
            <ScrollArea className="h-full">
              <AssetPanelBody
                assets={visibleAssets}
                folders={folders}
                rootAssetCount={rootAssets.length}
                currentFolder={currentFolder}
                dropActive={dropActive}
                creatingFolder={creatingFolder}
                renamingFolder={renamingFolder}
                folderError={folderError}
                pendingAssetDelete={pendingAssetDelete}
                pendingFolderDelete={pendingFolderDelete}
                onOpenFolder={setCurrentFolder}
                onView={openAssetTab}
                onDeleteAsset={(asset) => void requestDeleteAsset(asset)}
                onConfirmDeleteAsset={(asset) => void confirmDeleteAsset(asset)}
                onCancelDeleteAsset={() => setPendingAssetDelete(null)}
                onDeleteFolder={requestDeleteFolder}
                onConfirmDeleteFolder={(folder) => void confirmDeleteFolder(folder)}
                onCancelDeleteFolder={() => setPendingFolderDelete(null)}
                onMoveAsset={(assetId, folder) => void moveAssetToFolder(assetId, folder)}
                onCommitNewFolder={(name) => void commitNewFolder(name)}
                onCancelNewFolder={cancelNewFolder}
                onStartRenameFolder={setRenamingFolder}
                onCommitRenameFolder={(folder, name) => void commitRenameFolder(folder, name)}
                onCancelRenameFolder={cancelRenameFolder}
              />
            </ScrollArea>
          </div>
        </ContextMenuTrigger>
        <ContextMenuContent className="min-w-40">
          <ContextMenuItem onSelect={onNewFolder}>
            <Folder />
            New Folder
          </ContextMenuItem>
          <ContextMenuItem onSelect={() => void onImportClick()} disabled={nativeDialogOpen}>
            <Plus />
            Import
          </ContextMenuItem>
        </ContextMenuContent>
      </ContextMenu>
    </div>
  );
}

function AssetPanelBody({
  assets,
  folders,
  rootAssetCount,
  currentFolder,
  dropActive,
  creatingFolder,
  renamingFolder,
  folderError,
  pendingAssetDelete,
  pendingFolderDelete,
  onOpenFolder,
  onView,
  onDeleteAsset,
  onConfirmDeleteAsset,
  onCancelDeleteAsset,
  onDeleteFolder,
  onConfirmDeleteFolder,
  onCancelDeleteFolder,
  onMoveAsset,
  onCommitNewFolder,
  onCancelNewFolder,
  onStartRenameFolder,
  onCommitRenameFolder,
  onCancelRenameFolder,
}: {
  assets: AssetEntry[];
  folders: string[];
  rootAssetCount: number;
  currentFolder: string | null;
  dropActive: boolean;
  creatingFolder: boolean;
  renamingFolder: string | null;
  folderError: string | null;
  pendingAssetDelete: PendingAssetDelete | null;
  pendingFolderDelete: string | null;
  onOpenFolder(folder: string | null): void;
  onView(asset: AssetEntry): void;
  onDeleteAsset(asset: AssetEntry): void;
  onConfirmDeleteAsset(asset: AssetEntry): void;
  onCancelDeleteAsset(): void;
  onDeleteFolder(folder: string): void;
  onConfirmDeleteFolder(folder: string): void;
  onCancelDeleteFolder(): void;
  onMoveAsset(assetId: string, folder: string | null): void;
  onCommitNewFolder(name: string): void;
  onCancelNewFolder(): void;
  onStartRenameFolder(folder: string): void;
  onCommitRenameFolder(folder: string, name: string): void;
  onCancelRenameFolder(): void;
}) {
  const hasFolderTargets = currentFolder
    ? folders.length > 1 || rootAssetCount > 0
    : folders.length > 0;
  const empty = !creatingFolder && !hasFolderTargets && assets.length === 0 && rootAssetCount === 0;
  return (
    <div
      className={
        dropActive ? "min-h-full rounded-sm p-2 ring-2 ring-inset ring-ring" : "min-h-full p-2"
      }
      onDragOver={(event) => {
        if (event.dataTransfer.types.includes(ASSET_DND_MIME)) {
          event.preventDefault();
          event.dataTransfer.dropEffect = "move";
        }
      }}
      onDrop={(event) => {
        const payload = readAssetPayload(event.dataTransfer);
        if (!payload) {
          return;
        }
        event.preventDefault();
        onMoveAsset(payload.id, currentFolder);
      }}
    >
      <div className="mb-2 flex h-7 items-center gap-1 text-xs text-muted-foreground">
        <Button type="button" size="xs" variant="ghost" onClick={() => onOpenFolder(null)}>
          Root
        </Button>
        {currentFolder ? (
          <>
            <span>/</span>
            <span className="font-mono text-foreground">{currentFolder}</span>
          </>
        ) : null}
      </div>
      {folderError ? <p className="mb-2 px-1 text-[11px] text-destructive">{folderError}</p> : null}
      {empty ? (
        <p className="px-1 py-3 text-center text-xs italic text-muted-foreground">
          No assets yet. Import or drag-and-drop a model or texture.
        </p>
      ) : (
        <div
          className="grid gap-2"
          style={{ gridTemplateColumns: "repeat(auto-fill, minmax(72px, 1fr))" }}
        >
          {creatingFolder ? (
            <NewFolderTile onCommit={onCommitNewFolder} onCancel={onCancelNewFolder} />
          ) : null}
          {currentFolder ? (
            <FolderTile
              name="Root"
              count={rootAssetCount}
              onOpen={() => onOpenFolder(null)}
              onMoveAsset={(assetId) => onMoveAsset(assetId, null)}
            />
          ) : null}
          {folders
            .filter((folder) => folder !== currentFolder)
            .map((folder) => (
              <FolderTile
                key={folder}
                name={folder}
                count={assetsInFolderCount(folder)}
                editing={renamingFolder === folder}
                onOpen={() => onOpenFolder(folder)}
                onMoveAsset={(assetId) => onMoveAsset(assetId, folder)}
                onRename={() => onStartRenameFolder(folder)}
                onDelete={() => onDeleteFolder(folder)}
                confirmingDelete={pendingFolderDelete === folder}
                deleteBody={deleteFolderBody(folder)}
                onConfirmDelete={() => onConfirmDeleteFolder(folder)}
                onCancelDelete={onCancelDeleteFolder}
                onCommitRename={(name) => onCommitRenameFolder(folder, name)}
                onCancelRename={onCancelRenameFolder}
              />
            ))}
          {assets.map((asset) => (
            <AssetTile
              key={asset.id}
              entry={asset}
              onView={onView}
              onDelete={onDeleteAsset}
              confirmingDelete={pendingAssetDelete?.asset.id === asset.id}
              deleteBody={pendingAssetDelete?.body}
              onConfirmDelete={onConfirmDeleteAsset}
              onCancelDelete={onCancelDeleteAsset}
            />
          ))}
        </div>
      )}
    </div>
  );

  function assetsInFolderCount(folder: string): number {
    return useEditorStore.getState().assets.filter((asset) => asset.folder === folder).length;
  }

  function deleteFolderBody(folder: string): string {
    const count = assetsInFolderCount(folder);
    return `Moves ${count} asset${count === 1 ? "" : "s"} to Root.`;
  }
}

function NewFolderTile({ onCommit, onCancel }: { onCommit(name: string): void; onCancel(): void }) {
  const [value, setValue] = useState("");
  const inputRef = useRef<HTMLInputElement | null>(null);
  const settledRef = useRef(false);

  useEffect(() => {
    const frame = requestAnimationFrame(() => {
      inputRef.current?.focus();
      inputRef.current?.select();
    });
    return () => cancelAnimationFrame(frame);
  }, []);

  const commit = (): void => {
    if (settledRef.current) {
      return;
    }
    settledRef.current = true;
    onCommit(value);
    window.setTimeout(() => {
      settledRef.current = false;
    }, 100);
  };

  return (
    <div className="flex w-[72px] flex-col gap-1 rounded-md border border-ring bg-background p-1">
      <div className="flex aspect-square w-full items-center justify-center rounded-sm bg-muted">
        <Folder className="size-8 text-muted-foreground" />
      </div>
      <Input
        ref={inputRef}
        value={value}
        className="h-5 rounded-sm px-1 py-0 text-center font-mono text-[10px]"
        onChange={(event) => setValue(event.currentTarget.value)}
        onBlur={commit}
        onKeyDown={(event) => {
          if (event.key === "Enter") {
            event.preventDefault();
            commit();
          } else if (event.key === "Escape") {
            event.preventDefault();
            settledRef.current = true;
            onCancel();
          }
        }}
      />
    </div>
  );
}

function FolderTile({
  name,
  count,
  editing = false,
  onOpen,
  onMoveAsset,
  onRename,
  onDelete,
  confirmingDelete = false,
  deleteBody,
  onConfirmDelete,
  onCancelDelete,
  onCommitRename,
  onCancelRename,
}: {
  name: string;
  count: number;
  editing?: boolean;
  onOpen(): void;
  onMoveAsset(assetId: string): void;
  onRename?(): void;
  onDelete?(): void;
  confirmingDelete?: boolean;
  deleteBody?: string;
  onConfirmDelete?(): void;
  onCancelDelete?(): void;
  onCommitRename?(name: string): void;
  onCancelRename?(): void;
}) {
  const content = (
    <>
      <div className="flex aspect-square w-full items-center justify-center rounded-sm bg-muted">
        <Folder className="size-8 text-muted-foreground" />
      </div>
      {editing && onCommitRename && onCancelRename ? (
        <FolderNameInput initial={name} onCommit={onCommitRename} onCancel={onCancelRename} />
      ) : (
        <>
          <span className="truncate px-0.5 text-center text-[10px] leading-tight text-foreground">
            {name}
          </span>
          <span className="text-center font-mono text-[9px] leading-none text-muted-foreground">
            {count}
          </span>
        </>
      )}
    </>
  );

  const tile = (
    <button
      type="button"
      className="flex w-[72px] flex-col gap-1 rounded-md border border-border bg-background p-1 text-left transition-colors hover:border-ring hover:bg-accent/40"
      title={name}
      onDoubleClick={onOpen}
      onDragOver={(event) => {
        if (event.dataTransfer.types.includes(ASSET_DND_MIME)) {
          event.preventDefault();
          event.dataTransfer.dropEffect = "move";
        }
      }}
      onDrop={(event) => {
        const payload = readAssetPayload(event.dataTransfer);
        if (!payload) {
          return;
        }
        event.preventDefault();
        event.stopPropagation();
        onMoveAsset(payload.id);
      }}
    >
      {content}
    </button>
  );

  if ((!onRename && !onDelete) || editing) {
    return (
      <div className="relative w-[72px]">
        {tile}
        {confirmingDelete && onConfirmDelete && onCancelDelete ? (
          <DeleteConfirm
            title={`Delete ${name}?`}
            body={deleteBody ?? "Moves contained assets to Root."}
            onConfirm={onConfirmDelete}
            onCancel={onCancelDelete}
          />
        ) : null}
      </div>
    );
  }

  return (
    <div className="relative w-[72px]">
      <ContextMenu>
        <ContextMenuTrigger asChild>{tile}</ContextMenuTrigger>
        <ContextMenuContent className="min-w-32">
          {onRename ? (
            <ContextMenuItem onSelect={onRename}>
              <Pen />
              Rename
            </ContextMenuItem>
          ) : null}
          {onDelete ? (
            <>
              {onRename ? <ContextMenuSeparator /> : null}
              <ContextMenuItem
                variant="destructive"
                className="bg-destructive/10 text-destructive focus:bg-destructive focus:text-destructive-foreground"
                onSelect={onDelete}
              >
                <Trash />
                Delete
              </ContextMenuItem>
            </>
          ) : null}
        </ContextMenuContent>
      </ContextMenu>
      {confirmingDelete && onConfirmDelete && onCancelDelete ? (
        <DeleteConfirm
          title={`Delete ${name}?`}
          body={deleteBody ?? "Moves contained assets to Root."}
          onConfirm={onConfirmDelete}
          onCancel={onCancelDelete}
        />
      ) : null}
    </div>
  );
}

interface PendingAssetDelete {
  asset: AssetEntry;
  body: string;
}

function FolderNameInput({
  initial,
  onCommit,
  onCancel,
}: {
  initial: string;
  onCommit(name: string): void;
  onCancel(): void;
}) {
  const [value, setValue] = useState(initial);
  const inputRef = useRef<HTMLInputElement | null>(null);
  const settledRef = useRef(false);

  useEffect(() => {
    const frame = requestAnimationFrame(() => {
      inputRef.current?.focus();
      inputRef.current?.select();
    });
    return () => cancelAnimationFrame(frame);
  }, []);

  const commit = (): void => {
    if (settledRef.current) {
      return;
    }
    settledRef.current = true;
    onCommit(value);
    window.setTimeout(() => {
      settledRef.current = false;
    }, 100);
  };

  return (
    <Input
      ref={inputRef}
      value={value}
      className="h-5 rounded-sm px-1 py-0 text-center font-mono text-[10px]"
      onClick={(event) => event.stopPropagation()}
      onDoubleClick={(event) => event.stopPropagation()}
      onChange={(event) => setValue(event.currentTarget.value)}
      onBlur={commit}
      onKeyDown={(event) => {
        if (event.key === "Enter") {
          event.preventDefault();
          commit();
        } else if (event.key === "Escape") {
          event.preventDefault();
          settledRef.current = true;
          onCancel();
        }
      }}
    />
  );
}

async function moveImportedAsset(assetId: string, folder: string): Promise<void> {
  if (assetId !== "0") {
    await client.moveAsset(assetId, folder);
  }
}

/// Hit-test a physical-pixel drop position against the Assets panel's DOM rect.
/// (The drop event reports physical pixels; getBoundingClientRect is CSS pixels, so
/// we scale by devicePixelRatio.)
function isInsidePanel(position: { x: number; y: number }): boolean {
  const el = document.querySelector('[data-asset-panel="true"]');
  if (!el) {
    return false;
  }
  const rect = el.getBoundingClientRect();
  const scale = window.devicePixelRatio || 1;
  const cssX = position.x / scale;
  const cssY = position.y / scale;
  return cssX >= rect.left && cssX <= rect.right && cssY >= rect.top && cssY <= rect.bottom;
}
