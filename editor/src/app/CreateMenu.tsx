/// A reusable "Create" dropdown listing every spawn preset, wired to
/// `client.addEntity(preset)`. The engine auto-selects the new entity, so on
/// success we mirror that locally (optimistic `selectEntity`) and the reconcile
/// poll's sceneVersion bump refreshes the hierarchy list. Used here in the
/// Hierarchy header; phase-8's menu bar imports the same component.
import type { LucideIcon } from "lucide-react";
import {
  Box,
  Camera,
  CircleDashed,
  Flashlight,
  Lightbulb,
  Plus,
  Sun,
} from "lucide-react";
import { client, type EntityPreset } from "../control/client";
import { useEditorStore } from "../state/store";
import { Button } from "@/components/ui/button";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";

/// The single hand-maintained surface mapping menu labels to `add-entity`
/// presets (the preset union itself comes from the typed client / protocol).
/// Icons follow the C++ Lucide SVG mapping (editor_app.cppm:101-107): cube=Box,
/// point-light=Lightbulb, spot-light=Flashlight, camera=Camera (directional-light
/// uses Sun, empty uses a neutral marker). Exported so the phase-8 menu bar's
/// Create menu reuses the same list.
export const CREATE_PRESETS: { label: string; preset: EntityPreset; icon: LucideIcon }[] = [
  { label: "Empty", preset: "empty", icon: CircleDashed },
  { label: "Cube", preset: "cube", icon: Box },
  { label: "Point Light", preset: "point-light", icon: Lightbulb },
  { label: "Spot Light", preset: "spot-light", icon: Flashlight },
  { label: "Directional Light", preset: "directional-light", icon: Sun },
  { label: "Camera", preset: "camera", icon: Camera },
];

export function CreateMenu() {
  const phase = useEditorStore((s) => s.engineStatus.phase);
  const selectEntity = useEditorStore((s) => s.selectEntity);

  const ready = phase === "ready";

  const create = (preset: EntityPreset): void => {
    void client
      .addEntity(preset)
      .then((ref) => {
        selectEntity(ref.id);
      })
      .catch(() => {});
  };

  return (
    <DropdownMenu>
      <DropdownMenuTrigger asChild>
        <Button
          type="button"
          size="xs"
          variant="outline"
          disabled={!ready}
          title="Create entity"
        >
          <Plus />
          <span>Create</span>
        </Button>
      </DropdownMenuTrigger>
      <DropdownMenuContent align="end" className="min-w-40">
        {CREATE_PRESETS.map(({ label, preset, icon: Icon }) => (
          <DropdownMenuItem key={preset} onSelect={() => create(preset)}>
            <Icon className="size-4 text-muted-foreground" />
            {label}
          </DropdownMenuItem>
        ))}
      </DropdownMenuContent>
    </DropdownMenu>
  );
}
