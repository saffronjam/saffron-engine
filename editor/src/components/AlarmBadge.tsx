/// The topbar active-alarms badge: ambient awareness of firing perf alarms even with the
/// Stats panel closed. Red if any critical, amber if any warning; hidden when none.
/// Clicking it opens the Stats dashboard (which holds the alarm log).
import { TriangleAlert } from "lucide-react";
import { useEditorStore } from "../state/store";
import { Button } from "@/components/ui/button";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";

export function AlarmBadge() {
  const activeAlarms = useEditorStore((s) => s.activeAlarms);
  const setBottomTab = useEditorStore((s) => s.setBottomTab);

  if (activeAlarms.length === 0) {
    return null;
  }
  const critical = activeAlarms.some((a) => a.severity === "critical");
  const tone = critical ? "text-red-400" : "text-amber-400";

  return (
    <Tooltip>
      <TooltipTrigger asChild>
        <Button
          type="button"
          size="xs"
          variant="ghost"
          className={`gap-1 ${tone}`}
          onClick={() => setBottomTab("stats")}
          aria-label="Active performance alarms"
        >
          <TriangleAlert className="size-3.5" />
          <span className="font-mono text-[11px] tabular-nums">{activeAlarms.length}</span>
        </Button>
      </TooltipTrigger>
      <TooltipContent>
        {activeAlarms.length} active performance alarm{activeAlarms.length === 1 ? "" : "s"} — open
        Stats
      </TooltipContent>
    </Tooltip>
  );
}
