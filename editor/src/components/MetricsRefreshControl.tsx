/// The frame-time graph's settings control: a compact chip (range · bucket · refresh) that
/// opens a shadcn Popover with the three orthogonal knobs plus pause. Range = how far back to
/// show; Window = the bucket/group-by interval samples are averaged into (the smoothness knob);
/// Refresh = how often the metrics lane fetches. Pausing freezes the whole dashboard.
import { Gauge, Pause, Play } from "lucide-react";
import { useEditorStore } from "../state/store";
import { Button } from "@/components/ui/button";
import { Popover, PopoverContent, PopoverTrigger } from "@/components/ui/popover";
import { cn } from "@/lib/utils";

/// Range options (seconds), bounded by the client history ring (~5 min).
export const RANGE_SECONDS = [10, 30, 60, 120, 300];
/// Window / bucket (group-by) interval options (ms).
export const BUCKET_MS = [50, 100, 250, 500, 1000];
/// Auto-refresh interval options (ms).
const RATES = [250, 500, 1000, 2000, 5000];

function secLabel(sec: number): string {
  return sec < 60 ? `${sec}s` : `${sec / 60}m`;
}

function msLabel(ms: number): string {
  return ms < 1000 ? `${ms}ms` : `${ms / 1000}s`;
}

function ChoiceRow({
  label,
  options,
  value,
  format,
  onSelect,
}: {
  label: string;
  options: number[];
  value: number;
  format: (v: number) => string;
  onSelect: (v: number) => void;
}) {
  return (
    <div className="flex flex-col gap-1">
      <span className="text-[10px] text-muted-foreground">{label}</span>
      <div className="grid grid-cols-5 gap-1">
        {options.map((o) => (
          <Button
            key={o}
            type="button"
            size="sm"
            variant={value === o ? "default" : "ghost"}
            className="h-6 px-0.5 font-mono text-[10px] tabular-nums"
            onClick={() => onSelect(o)}
          >
            {format(o)}
          </Button>
        ))}
      </div>
    </div>
  );
}

export function MetricsRefreshControl() {
  const rangeSec = useEditorStore((s) => s.metricsRangeSec);
  const bucketMs = useEditorStore((s) => s.metricsBucketMs);
  const refreshMs = useEditorStore((s) => s.metricsRefreshMs);
  const paused = useEditorStore((s) => s.metricsPaused);
  const setMetricsRangeSec = useEditorStore((s) => s.setMetricsRangeSec);
  const setMetricsBucketMs = useEditorStore((s) => s.setMetricsBucketMs);
  const setMetricsRefreshMs = useEditorStore((s) => s.setMetricsRefreshMs);
  const setMetricsPaused = useEditorStore((s) => s.setMetricsPaused);

  return (
    <Popover>
      <PopoverTrigger asChild>
        <button
          type="button"
          aria-label="Frame-time graph settings"
          className={cn(
            "flex h-6 items-center gap-1 rounded-md border border-input px-1.5 font-mono text-[10px] tabular-nums",
            paused ? "text-amber-400" : "text-muted-foreground hover:text-foreground",
          )}
        >
          {paused ? <Pause className="size-3" /> : <Gauge className="size-3" />}
          {paused
            ? "paused"
            : `${secLabel(rangeSec)} · ${msLabel(bucketMs)} · ${msLabel(refreshMs)}`}
        </button>
      </PopoverTrigger>
      <PopoverContent align="end" sideOffset={4} className="w-52 p-2">
        <div className="flex flex-col gap-2.5">
          <ChoiceRow
            label="Range"
            options={RANGE_SECONDS}
            value={rangeSec}
            format={secLabel}
            onSelect={setMetricsRangeSec}
          />
          <ChoiceRow
            label="Window"
            options={BUCKET_MS}
            value={bucketMs}
            format={msLabel}
            onSelect={setMetricsBucketMs}
          />
          <ChoiceRow
            label="Refresh"
            options={RATES}
            value={refreshMs}
            format={msLabel}
            onSelect={setMetricsRefreshMs}
          />
          <Button
            type="button"
            size="sm"
            variant={paused ? "default" : "outline"}
            className="h-7 w-full justify-start gap-1.5 text-[11px]"
            onClick={() => setMetricsPaused(!paused)}
          >
            {paused ? <Play className="size-3.5" /> : <Pause className="size-3.5" />}
            {paused ? "Resume" : "Pause"}
          </Button>
        </div>
      </PopoverContent>
    </Popover>
  );
}
