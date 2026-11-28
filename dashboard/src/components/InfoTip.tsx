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
        // Escape closes the tip first; it must not also close a dialog the tip sits in. A native
        // dialog treats any Escape keydown that was not cancelled as a close request, so stopping
        // propagation alone is not enough.
        onKeyDown={(event) => {
          if (event.key === 'Escape' && open) {
            event.preventDefault();
            event.stopPropagation();
            setOpen(false);
          }
        }}
        type="button"
      >
        <Info size={12} />
      </button>
      <span className="info-tip-text" id={id} role="tooltip">{text}</span>
    </span>
  );
}
