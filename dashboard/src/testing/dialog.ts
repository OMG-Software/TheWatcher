import { fireEvent } from '@testing-library/react';

/**
 * jsdom has `HTMLDialogElement` but not `showModal()`, `close()` or close requests. This models
 * the parts of the browser's behaviour that `Modal` relies on, closely enough to test it:
 *
 * - `showModal()` sets `open` and puts the dialog on top of a stack of modal dialogs. It throws
 *   when the dialog is already open or not in the document, like a browser.
 * - `close()` clears `open` and fires `close`. (A browser fires `close` in a later task; here it
 *   fires at once.)
 * - An Escape `keydown` that nothing cancelled is a close request for the topmost modal dialog:
 *   it gets a cancelable `cancel` event and closes unless that is cancelled. A browser decides
 *   this after the keydown has been dispatched, so stopping propagation does not prevent it
 *   (only `preventDefault()` does). That is why the check runs after `dispatchEvent` returns.
 *
 * It does not make the rest of the page inert, put the dialog in the top layer, or draw a backdrop.
 */
export function installDialogShim() {
  if (typeof HTMLDialogElement === 'undefined' || typeof HTMLDialogElement.prototype.showModal === 'function') return;

  const modals: HTMLDialogElement[] = [];
  const topModal = () => modals.filter((dialog) => dialog.isConnected && dialog.open).at(-1);

  HTMLDialogElement.prototype.showModal = function showModal(this: HTMLDialogElement) {
    if (this.open) throw new DOMException('The dialog is already open.', 'InvalidStateError');
    if (!this.isConnected) throw new DOMException('The dialog is not in a document.', 'InvalidStateError');
    this.setAttribute('open', '');
    modals.push(this);
  };

  HTMLDialogElement.prototype.close = function close(this: HTMLDialogElement) {
    if (!this.open) return;
    this.removeAttribute('open');
    modals.splice(modals.indexOf(this), 1);
    this.dispatchEvent(new Event('close'));
  };

  const dispatch = EventTarget.prototype.dispatchEvent;
  EventTarget.prototype.dispatchEvent = function dispatchEvent(this: EventTarget, event: Event) {
    const result = dispatch.call(this, event);
    if (event.type === 'keydown' && (event as KeyboardEvent).key === 'Escape' && !event.defaultPrevented) {
      const dialog = topModal();
      if (dialog && dialog.dispatchEvent(new Event('cancel', { cancelable: true }))) dialog.close();
    }
    return result;
  };
}

/** A point just outside the dialog's box, where the browser's `::backdrop` is. */
function outside(dialog: HTMLElement) {
  const box = dialog.getBoundingClientRect();
  return { clientX: box.right + 1, clientY: box.bottom + 1 };
}

/** Press and release on the backdrop. A browser sends both to the dialog element, outside its box. */
export function clickBackdrop(dialog: HTMLElement) {
  fireEvent.pointerDown(dialog, outside(dialog));
  fireEvent.click(dialog, outside(dialog));
}

/** Press inside the dialog and release on the backdrop, as when dragging a text selection out of it. */
export function dragOntoBackdrop(dialog: HTMLElement, from: HTMLElement) {
  fireEvent.pointerDown(from);
  // The click goes to the nearest element that holds both the press and the release: the dialog.
  fireEvent.click(dialog, outside(dialog));
}
