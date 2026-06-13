/// The topbar active-alarms badge: ambient awareness of firing perf alarms even with the
/// Stats panel closed. Red if any critical, amber if any warning; hidden when none.
/// Clicking it opens the Stats dashboard (which holds the alarm log).
import { TriangleAlert } from "lucide-react";
import { useEditorStore } from "../state/store";
import { Button } from "@/components/ui/button";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";

export function AlarmBadge() {
  const activeAlarms = useEditorStore((s) => s.activeAlarms);
  const openPanel = useEditorStore((s) => s.openPanel);

  if (activeAlarms.length === 0) {
    return null;
  }
  const critical = activeAlarms.some((a) => a.severity === "critical");
  const tone = critical ? "text-red-400" : "text-amber-400";
  // A per-pass alarm (non-empty `pass`) is a GPU-pass issue — land on the Profiler, where the
  // user can capture and drill into the offending pass. Frame-wide alarms open Stats.
  const passAlarm = activeAlarms.some((a) => a.pass !== "");
  const target = passAlarm ? "profiler" : "stats";

  return (
    <Tooltip>
      <TooltipTrigger asChild>
        <Button
          type="button"
          size="xs"
          variant="ghost"
          className={`gap-1 ${tone}`}
          onClick={() => openPanel(target)}
          aria-label="Active performance alarms"
        >
          <TriangleAlert className="size-3.5" />
          <span className="font-mono text-[11px] tabular-nums">{activeAlarms.length}</span>
        </Button>
      </TooltipTrigger>
      <TooltipContent>
        {activeAlarms.length} active performance alarm{activeAlarms.length === 1 ? "" : "s"} — open
        {passAlarm ? " Profiler" : " Stats"}
      </TooltipContent>
    </Tooltip>
  );
}
