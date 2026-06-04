/// Custom Tauri titlebar with editor view tabs.
import { getCurrentWindow } from "@tauri-apps/api/window";
import { Box, File, Image as ImageIcon, Maximize2, Minus, Square, X } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import type { MouseEvent, PointerEvent, ReactNode } from "react";
import { useEditorStore, type ViewTab } from "../state/store";
import { Button } from "@/components/ui/button";
import { cn } from "@/lib/utils";

const appWindow = getCurrentWindow();
const TAB_DRAG_THRESHOLD_PX = 4;

interface TabDragState {
  id: string;
  startX: number;
  currentX: number;
  dragging: boolean;
}

export function WindowTitlebar() {
  const [maximized, setMaximized] = useState(false);
  const [tabDrag, setTabDrag] = useState<TabDragState | null>(null);
  const tabRefs = useRef(new Map<string, HTMLButtonElement>());
  const tabs = useEditorStore((s) => s.viewTabs);
  const activeTabId = useEditorStore((s) => s.activeViewTabId);
  const setActiveViewTab = useEditorStore((s) => s.setActiveViewTab);
  const closeViewTab = useEditorStore((s) => s.closeViewTab);
  const moveViewTab = useEditorStore((s) => s.moveViewTab);

  useEffect(() => {
    let cancelled = false;

    const syncMaximized = async (): Promise<void> => {
      const nextMaximized = await appWindow.isMaximized();
      if (!cancelled) {
        setMaximized(nextMaximized);
      }
    };

    void syncMaximized();
    const unlisten = appWindow.onResized(() => {
      void syncMaximized();
    });

    return () => {
      cancelled = true;
      void unlisten.then((off) => off());
    };
  }, []);

  const minimize = (): void => {
    void appWindow.minimize();
  };

  const toggleMaximize = async (): Promise<void> => {
    await appWindow.toggleMaximize();
    setMaximized(await appWindow.isMaximized());
  };

  const close = (): void => {
    void appWindow.close();
  };

  const beginTitlebarDrag = (event: MouseEvent<HTMLElement>): void => {
    if (event.button !== 0) {
      return;
    }

    const target = event.target;
    if (target instanceof Element && target.closest("[data-titlebar-control='true']")) {
      return;
    }

    if (event.detail === 2) {
      void toggleMaximize();
      return;
    }

    void appWindow.startDragging();
  };

  const setTabRef = (id: string, node: HTMLButtonElement | null): void => {
    if (node) {
      tabRefs.current.set(id, node);
    } else {
      tabRefs.current.delete(id);
    }
  };

  const insertionIndexForPointer = (movingId: string, x: number): number => {
    const withoutMoving = tabs.filter((tab) => tab.id !== movingId);
    for (let i = 0; i < withoutMoving.length; i += 1) {
      const tab = withoutMoving[i];
      if (tab.id === "scene") {
        continue;
      }
      const node = tabRefs.current.get(tab.id);
      if (!node) {
        continue;
      }
      const rect = node.getBoundingClientRect();
      if (x < rect.left + rect.width / 2) {
        return i;
      }
    }
    return withoutMoving.length;
  };

  const beginTabDrag = (tab: ViewTab, event: PointerEvent<HTMLButtonElement>): void => {
    if (!tab.closable || event.button !== 0) {
      return;
    }
    const target = event.target;
    if (target instanceof Element && target.closest("[data-tab-close='true']")) {
      return;
    }
    event.preventDefault();
    event.currentTarget.setPointerCapture(event.pointerId);
    setTabDrag({ id: tab.id, startX: event.clientX, currentX: event.clientX, dragging: false });
  };

  const moveTabDrag = (event: PointerEvent<HTMLButtonElement>): void => {
    if (!tabDrag) {
      return;
    }
    const delta = event.clientX - tabDrag.startX;
    const dragging = tabDrag.dragging || Math.abs(delta) >= TAB_DRAG_THRESHOLD_PX;
    setTabDrag({ ...tabDrag, currentX: event.clientX, dragging });
    if (dragging) {
      moveViewTab(tabDrag.id, insertionIndexForPointer(tabDrag.id, event.clientX));
    }
  };

  const endTabDrag = (tab: ViewTab, event: PointerEvent<HTMLButtonElement>): void => {
    if (!tabDrag || tabDrag.id !== tab.id) {
      return;
    }
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
    const activate = !tabDrag.dragging;
    setTabDrag(null);
    if (activate) {
      setActiveViewTab(tab.id);
    }
  };

  return (
    <header
      className="flex h-9 flex-none items-center border-b border-border bg-card"
      data-tauri-drag-region
      onMouseDown={beginTitlebarDrag}
    >
      <div
        className="flex h-full min-w-0 flex-none items-end px-2"
        role="tablist"
        data-titlebar-control="true"
      >
        {tabs.map((tab) => (
          <TitlebarTab
            key={tab.id}
            tab={tab}
            active={tab.id === activeTabId}
            dragging={tabDrag?.id === tab.id && tabDrag.dragging}
            setRef={(node) => setTabRef(tab.id, node)}
            onActivate={() => setActiveViewTab(tab.id)}
            onClose={() => closeViewTab(tab.id)}
            onPointerDown={(event) => beginTabDrag(tab, event)}
            onPointerMove={moveTabDrag}
            onPointerUp={(event) => endTabDrag(tab, event)}
            onPointerCancel={() => setTabDrag(null)}
          />
        ))}
      </div>
      <div className="min-w-0 flex-1 self-stretch" data-tauri-drag-region />
      <div
        className="flex w-32 flex-none justify-end"
        data-tauri-drag-region="false"
        data-titlebar-control="true"
      >
        <TitlebarButton label="Minimize" onClick={minimize}>
          <Minus />
        </TitlebarButton>
        <TitlebarButton
          label={maximized ? "Restore" : "Maximize"}
          onClick={() => void toggleMaximize()}
        >
          {maximized ? <Square /> : <Maximize2 />}
        </TitlebarButton>
        <TitlebarButton label="Close" onClick={close} variant="close">
          <X />
        </TitlebarButton>
      </div>
    </header>
  );
}

type TitlebarTabProps = {
  active: boolean;
  tab: ViewTab;
  dragging: boolean;
  setRef(node: HTMLButtonElement | null): void;
  onActivate(): void;
  onClose(): void;
  onPointerDown(event: PointerEvent<HTMLButtonElement>): void;
  onPointerMove(event: PointerEvent<HTMLButtonElement>): void;
  onPointerUp(event: PointerEvent<HTMLButtonElement>): void;
  onPointerCancel(): void;
};

function TitlebarTab({
  active,
  tab,
  dragging,
  setRef,
  onActivate,
  onClose,
  onPointerDown,
  onPointerMove,
  onPointerUp,
  onPointerCancel,
}: TitlebarTabProps) {
  const Icon = tabIcon(tab);
  return (
    <button
      ref={setRef}
      type="button"
      className={cn(
        "flex h-8 min-w-28 max-w-48 items-center gap-2 rounded-t-md border px-3 text-left text-sm font-medium",
        "transition-[transform,margin,background-color,border-color,color] duration-150 ease-out",
        tab.closable ? "cursor-default" : "cursor-pointer",
        dragging && "relative z-10 cursor-grabbing shadow-lg",
        active
          ? "border-border border-b-background bg-background text-foreground"
          : "border-transparent text-muted-foreground hover:bg-accent hover:text-accent-foreground",
      )}
      aria-selected={active}
      role="tab"
      title={tab.title}
      onClick={() => {
        if (!tab.closable) {
          onActivate();
        }
      }}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerCancel={onPointerCancel}
    >
      <Icon className="size-4 flex-none" />
      <span className="min-w-0 flex-1 truncate">{tab.title}</span>
      {tab.closable ? (
        <span
          role="button"
          tabIndex={0}
          aria-label={`Close ${tab.title}`}
          data-tab-close="true"
          className="flex size-5 flex-none items-center justify-center rounded-sm hover:bg-accent"
          onClick={(event) => {
            event.stopPropagation();
            onClose();
          }}
          onKeyDown={(event) => {
            if (event.key === "Enter" || event.key === " ") {
              event.preventDefault();
              onClose();
            }
          }}
        >
          <X className="size-3.5" />
        </span>
      ) : null}
    </button>
  );
}

function tabIcon(tab: ViewTab) {
  if (tab.kind === "scene") {
    return Box;
  }
  if (tab.assetType === "mesh") {
    return Box;
  }
  if (tab.assetType === "texture") {
    return ImageIcon;
  }
  return File;
}

type TitlebarButtonProps = {
  children: ReactNode;
  label: string;
  onClick: () => void;
  variant?: "default" | "close";
};

function TitlebarButton({ children, label, onClick, variant = "default" }: TitlebarButtonProps) {
  const className =
    variant === "close"
      ? "h-9 w-11 rounded-none hover:bg-destructive hover:text-destructive-foreground"
      : "h-9 w-11 rounded-none hover:bg-accent";

  return (
    <Button
      type="button"
      size="icon-sm"
      variant="ghost"
      className={className}
      aria-label={label}
      title={label}
      onClick={onClick}
    >
      {children}
    </Button>
  );
}
