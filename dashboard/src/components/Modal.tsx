import React from 'react';
import { X } from 'lucide-react';

const FOCUSABLE = 'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])';

/**
 * Dialog behaviour: focus moves in on open, Tab stays inside, Escape closes, and focus returns to
 * what had it before. Spread the returned key handler on the dialog element. While `dismissible`
 * is false (a request is running) Escape does nothing.
 *
 * Focus goes to the element marked `data-autofocus`, else to the first control after the header,
 * else the first control. The opener is read during the first render, before any child can take
 * focus, so it is restored correctly even for dialogs that focus an input on open.
 */
export function useDialog(panelRef: React.RefObject<HTMLElement | null>, dismissible: boolean, onClose: () => void) {
  const [opener] = React.useState(() => (document.activeElement instanceof HTMLElement ? document.activeElement : null));
  // The latest values, read by the key handler without re-running the focus effect.
  const closeRef = React.useRef({ dismissible, onClose });
  React.useEffect(() => {
    closeRef.current = { dismissible, onClose };
  });

  React.useEffect(() => {
    const panel = panelRef.current;
    if (panel) {
      const controls = [...panel.querySelectorAll<HTMLElement>(FOCUSABLE)];
      const target =
        panel.querySelector<HTMLElement>('[data-autofocus]') ??
        controls.find((control) => !control.closest('.modal-header')) ??
        controls[0] ??
        panel;
      target.focus();
    }
    return () => opener?.focus();
  }, [opener, panelRef]);

  return (event: React.KeyboardEvent) => {
    const panel = panelRef.current;
    if (event.key === 'Escape') {
      event.stopPropagation();
      if (closeRef.current.dismissible) closeRef.current.onClose();
      return;
    }
    if (event.key !== 'Tab' || !panel) return;
    const items = [...panel.querySelectorAll<HTMLElement>(FOCUSABLE)];
    if (items.length === 0) {
      event.preventDefault();
      return;
    }
    const first = items[0];
    const last = items[items.length - 1];
    const active = document.activeElement;
    if (event.shiftKey && (active === first || !panel.contains(active))) {
      event.preventDefault();
      last.focus();
    } else if (!event.shiftKey && (active === last || !panel.contains(active))) {
      event.preventDefault();
      first.focus();
    }
  };
}

/**
 * An accessible dialog: `role="dialog"` with a name, `aria-modal`, the behaviour of `useDialog`,
 * and a click on the backdrop to close. While `dismissible` is false (a request is running)
 * none of the ways to close work, so the failure of that request still has somewhere to show.
 */
export function Modal({
  children,
  className = '',
  closeLabel = 'Close',
  dismissible = true,
  onClose,
  title,
  titleContent,
}: {
  children: React.ReactNode;
  className?: string;
  closeLabel?: string;
  dismissible?: boolean;
  onClose: () => void;
  /** The dialog's accessible name. */
  title: string;
  /** What the header shows when it should be more than the plain title. */
  titleContent?: React.ReactNode;
}) {
  const panelRef = React.useRef<HTMLDivElement>(null);
  const titleId = React.useId();
  const onKeyDown = useDialog(panelRef, dismissible, onClose);

  return (
    <div className="modal-backdrop" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget && dismissible) onClose(); }}>
      <div aria-labelledby={titleId} aria-modal="true" className={`modal-panel ${className}`} onKeyDown={onKeyDown} ref={panelRef} role="dialog" tabIndex={-1}>
        <div className="modal-header">
          <h2 id={titleId}>{titleContent ?? title}</h2>
          <button aria-label={closeLabel} className="icon-button" disabled={!dismissible} onClick={onClose} type="button"><X size={14} /></button>
        </div>
        {children}
      </div>
    </div>
  );
}
