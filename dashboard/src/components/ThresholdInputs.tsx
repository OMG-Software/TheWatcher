import type { AnomalyConfig } from '../models';
import { DEFAULT_NETWORK_THRESHOLDS, DEFAULT_PERCENT_THRESHOLDS } from '../status';

export function NumberField({
  help,
  label,
  max,
  min,
  onChange,
  step = 1,
  value,
}: {
  help?: string;
  label: string;
  max?: number;
  min: number;
  onChange: (value: number) => void;
  step?: number;
  value: number;
}) {
  return (
    <label title={help}>
      {label}
      <input max={max} min={min} step={step} type="number" value={value} onChange={(event) => onChange(Number(event.target.value))} />
    </label>
  );
}

export function AnomalyInputs({
  anomaly,
  onChange,
}: {
  anomaly: AnomalyConfig;
  onChange: (field: keyof AnomalyConfig, value: number) => void;
}) {
  return (
    <div className="anomaly-inputs">
      <NumberField help="Multiplier above the learned baseline required before anomaly status is raised. Zero disables anomaly detection." label="Anomaly multiplier (0=off)" min={0} step={0.1} value={anomaly.multiplier} onChange={(value) => onChange('multiplier', value)} />
      <NumberField help="Number of hours of recent samples used to calculate the normal baseline for anomaly detection." label="Baseline hours" min={1} value={anomaly.baseline_window_hours} onChange={(value) => onChange('baseline_window_hours', value)} />
    </div>
  );
}

export function ThresholdInputs({
  onChange,
  thresholds,
}: {
  onChange: (field: keyof typeof DEFAULT_PERCENT_THRESHOLDS, value: number) => void;
  thresholds: typeof DEFAULT_PERCENT_THRESHOLDS;
}) {
  return (
    <div className="threshold-inputs">
      <NumberField help="Usage percent that raises warning status for this item." label="Warn" min={1} value={thresholds.warning_percent} onChange={(value) => onChange('warning_percent', value)} />
      <NumberField help="Usage percent that raises degraded status for this item." label="Degrade" min={1} value={thresholds.degraded_percent} onChange={(value) => onChange('degraded_percent', value)} />
      <NumberField help="Usage percent that raises critical status for this item." label="Critical" min={1} value={thresholds.critical_percent} onChange={(value) => onChange('critical_percent', value)} />
    </div>
  );
}

export function ThresholdRow({
  label,
  onChange,
  thresholds,
}: {
  label: string;
  onChange: (field: keyof typeof DEFAULT_PERCENT_THRESHOLDS, value: number) => void;
  thresholds: typeof DEFAULT_PERCENT_THRESHOLDS;
}) {
  return (
    <div className="threshold-row">
      <span>{label}</span>
      <ThresholdInputs thresholds={thresholds} onChange={onChange} />
    </div>
  );
}

export function NetworkThresholdInputs({
  onChange,
  thresholds,
}: {
  onChange: (field: keyof typeof DEFAULT_NETWORK_THRESHOLDS, value: number) => void;
  thresholds: typeof DEFAULT_NETWORK_THRESHOLDS;
}) {
  return (
    <div className="threshold-inputs">
      <NumberField help="Network throughput in megabits per second that raises warning status." label="Warn Mb/s" min={1} value={thresholds.warning_mbps} onChange={(value) => onChange('warning_mbps', value)} />
      <NumberField help="Network throughput in megabits per second that raises degraded status." label="Degrade Mb/s" min={1} value={thresholds.degraded_mbps} onChange={(value) => onChange('degraded_mbps', value)} />
      <NumberField help="Network throughput in megabits per second that raises critical status." label="Critical Mb/s" min={1} value={thresholds.critical_mbps} onChange={(value) => onChange('critical_mbps', value)} />
    </div>
  );
}

