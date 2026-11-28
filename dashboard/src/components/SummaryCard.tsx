import type { HealthColor } from '../models';

export function SummaryCard({ label, value, color }: { label: string; value: number; color: HealthColor }) {
  return (
    <div className="summary-card">
      <div className="summary-label">{label}</div>
      <div className={`summary-value ${color}`}>{value}</div>
    </div>
  );
}

