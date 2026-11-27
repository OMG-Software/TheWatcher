import type { HealthColor } from '../models';

export function StatusDot({ color, label, acknowledged = false }: { color: HealthColor; label: string; acknowledged?: boolean }) {
  return (
    <span className="state-note">
      <span className={`dot ${color}${acknowledged ? ' acknowledged' : ''}`} aria-label={acknowledged ? `${label} (acknowledged)` : undefined} />
      {' '}{label}
      {acknowledged ? <span className="acknowledged-tag" aria-hidden="true">ack</span> : null}
    </span>
  );
}

