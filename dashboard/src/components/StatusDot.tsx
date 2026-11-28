import type { HealthColor } from '../models';

export function StatusDot({ color, label, acknowledged = false }: { color: HealthColor; label: string; acknowledged?: boolean }) {
  return (
    <span className="state-note">
      <span aria-hidden="true" className={`dot ${color}${acknowledged ? ' acknowledged' : ''}`} />
      {' '}{label}
      {acknowledged ? <span className="acknowledged-tag">ack</span> : null}
    </span>
  );
}

