import React from 'react';
import { ActionFailureContext } from '../hooks/useRunAction';
import { ActionError } from './ActionError';
import { Modal } from './Modal';

/**
 * Asks before a destructive or hard-to-undo action. The message must name what is affected
 * (and how many, for bulk actions). `onConfirm` returns true once the action succeeded, and
 * only then does the dialog close; on failure it stays open and shows the error under `errorKey`.
 * With `reasonLabel` it also collects an optional free-text reason and passes it to `onConfirm`.
 */
export function ConfirmDialog({
  busy,
  confirmLabel,
  danger = false,
  errorKey,
  message,
  onCancel,
  onConfirm,
  reasonLabel,
  title,
}: {
  busy: boolean;
  confirmLabel: string;
  danger?: boolean;
  errorKey: string;
  message: React.ReactNode;
  onCancel: () => void;
  onConfirm: (reason: string) => Promise<boolean>;
  reasonLabel?: string;
  title: string;
}) {
  const [reason, setReason] = React.useState('');
  const { clear } = React.useContext(ActionFailureContext);
  const cancel = () => {
    if (busy) return;
    clear(errorKey);
    onCancel();
  };
  return (
    <Modal dismissible={!busy} onClose={cancel} title={title}>
      <p className="confirm-message">{message}</p>
      {reasonLabel ? (
        <label>
          {reasonLabel} <span className="field-hint">(optional)</span>
          <input data-autofocus maxLength={500} value={reason} onChange={(event) => setReason(event.target.value)} />
        </label>
      ) : null}
      <ActionError actionKey={errorKey} />
      <div className="button-row modal-actions">
        <button className="text-button secondary" disabled={busy} onClick={cancel} type="button">Cancel</button>
        <button
          data-autofocus={reasonLabel ? undefined : true}
          className={`text-button ${danger ? 'danger' : ''}`}
          disabled={busy}
          onClick={() => void onConfirm(reason).then((done) => { if (done) onCancel(); })}
          type="button"
        >
          {confirmLabel}
        </button>
      </div>
    </Modal>
  );
}
