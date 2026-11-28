import React from 'react';
import { X } from 'lucide-react';

const FOCUSABLE = 'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])';

/** True when a mouse event on the dialog element landed outside its box, i.e. on its `::backdrop`. */
function onBackdrop(dialog: HTMLDialogElement, event: MouseEvent) {
  if (event.target !== dialog) return false;
  const box = dialog.getBoundingClientRect();
  return event.clientX < box.left || event.clientX > box.right || event.clientY < box.top || event.clientY > box.bottom;
}

/**
 * The dashboard's dialog: a native `<dialog>` opened with `showModal()`. The browser puts it in
 * the top layer, draws its `::backdrop`, makes the rest of the page inert (so Tab cannot reach
 * the page behind it) and turns Escape into a `cancel` event.
 *
 * The parent controls it: render it to open it, stop rendering it to close it. Escape, a click on
 * the backdrop and the close button only call `onClose`; the dialog stays open until the parent
 * removes it. While `dismissible` is false (a request is running) none of them do anything, so the
 * failure of that request still has somewhere to show. A control inside that handles Escape
 * itself must call `preventDefault()`, or the browser also treats the key as a close request.
 *
 * Focus moves in on open: to the element marked `data-autofocus` (not React's `autoFocus`), else
 * the first control after the header, else the first control. When the dialog goes away, focus
 * returns to whatever had it before; that is read during the first render, before any child can
 * take focus.
 */
export function Modal({
  children,
  className = '',
  closeLabel = 'Close',
  closeOnBackdrop = true,
  dismissible = true,
  onClose,
  title,
  titleContent,
}: {
  children: React.ReactNode;
  className?: string;
  closeLabel?: string;
  /** False for dialogs whose edits a stray click must not throw away. */
  closeOnBackdrop?: boolean;
  dismissible?: boolean;
  onClose: () => void;
  /** The dialog's accessible name, and its heading unless `titleContent` is given. */
  title: string;
  /** What the header shows instead of the `<h2>` title. The dialog is still named by `title`. */
  titleContent?: React.ReactNode;
}) {
  const dialogRef = React.useRef<HTMLDialogElement>(null);
  const titleId = React.useId();
  const [opener] = React.useState(() => (document.activeElement instanceof HTMLElement ? document.activeElement : null));

  // Called from the native listeners below, always with the latest props.
  const requestClose = React.useEffectEvent((fromBackdrop: boolean) => {
    if (dismissible && (closeOnBackdrop || !fromBackdrop)) onClose();
  });
  const isDismissible = React.useEffectEvent(() => dismissible);

  // A layout effect, so the dialog is open and focused before the browser first paints it.
  React.useLayoutEffect(() => {
    const dialog = dialogRef.current;
    if (!dialog) return undefined;
    if (!dialog.open) dialog.showModal();

    const controls = [...dialog.querySelectorAll<HTMLElement>(FOCUSABLE)];
    const target =
      dialog.querySelector<HTMLElement>('[data-autofocus]') ??
      controls.find((control) => !control.closest('.modal-header')) ??
      controls[0] ??
      dialog;
    target.focus();

    // Escape (or another close request): keep the dialog open and let the parent decide.
    const onCancel = (event: Event) => {
      event.preventDefault();
      requestClose(false);
    };
    // A browser may close it anyway: a second close request with no click in between cannot be
    // cancelled. While the parent still renders it, it must stay open, so open it again.
    const onNativeClose = () => {
      if (!dialog.open) dialog.showModal();
    };
    // While it cannot be dismissed, cancel the Escape key itself so the browser makes no close
    // request at all. Otherwise a quick second Escape during a save hides the dialog for a moment
    // until `onNativeClose` brings it back.
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape' && !isDismissible()) event.preventDefault();
    };
    // The press and the release must both land on the backdrop. A text selection dragged out of
    // the dialog also ends in a click on the dialog element, and must not close it.
    let pressedOnBackdrop = false;
    const onPointerDown = (event: PointerEvent) => {
      pressedOnBackdrop = onBackdrop(dialog, event);
    };
    const onClick = (event: MouseEvent) => {
      if (pressedOnBackdrop && onBackdrop(dialog, event)) requestClose(true);
      pressedOnBackdrop = false;
    };

    dialog.addEventListener('cancel', onCancel);
    dialog.addEventListener('close', onNativeClose);
    dialog.addEventListener('keydown', onKeyDown);
    dialog.addEventListener('pointerdown', onPointerDown);
    dialog.addEventListener('click', onClick);
    return () => {
      dialog.removeEventListener('cancel', onCancel);
      dialog.removeEventListener('close', onNativeClose);
      dialog.removeEventListener('keydown', onKeyDown);
      dialog.removeEventListener('pointerdown', onPointerDown);
      dialog.removeEventListener('click', onClick);
      if (dialog.open) dialog.close();
      opener?.focus();
    };
  }, [opener]);

  return (
    <dialog
      aria-label={titleContent ? title : undefined}
      aria-labelledby={titleContent ? undefined : titleId}
      className={`modal-panel ${className}`}
      ref={dialogRef}
    >
      <div className="modal-header">
        {titleContent ?? <h2 id={titleId}>{title}</h2>}
        <button aria-label={closeLabel} className="icon-button" disabled={!dismissible} onClick={onClose} type="button"><X size={14} /></button>
      </div>
      {children}
    </dialog>
  );
}
