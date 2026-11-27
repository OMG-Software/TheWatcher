import React from 'react';
import { Info } from 'lucide-react';

/**
 * Help text that keyboard and touch users can reach: a focusable button that toggles a
 * description (hover also shows it). Escape closes it. The help is tied to the button with
 * `aria-describedby`, so a screen reader reads it with the control.
 */
export function InfoTip({ label, text }: { label: string; text: string }) {
  const [open, setOpen] = React.useState(false);
  const id = React.useId();
  return (
    <span className={`info-tip ${open ? 'open' : ''}`}>
      <button
        aria-describedby={id}
        aria-expanded={open}
        aria-label={`About ${label}`}
        className="info-tip-button"
        onBlur={() => setOpen(false)}
        onClick={() => setOpen((current) => !current)}
        onKeyDown={(event) => { if (event.key === 'Escape') setOpen(false); }}
        type="button"
      >
        <Info size={12} />
      </button>
      <span className="info-tip-text" id={id} role="tooltip">{text}</span>
    </span>
  );
}
