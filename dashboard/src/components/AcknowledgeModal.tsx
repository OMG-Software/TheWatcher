import React from 'react';
import { ActionFailureContext } from '../hooks/useRunAction';
import { ActionError } from './ActionError';
import { Modal } from './Modal';

/**
 * Stays open (with the note) until the parent closes it, so a failed save loses nothing; the parent shows the failure under `errorKey`.
 * It cannot be dismissed while the request runs (the failure would have nowhere to show), and dismissing it drops its old failure.
 */
export function AcknowledgeModal({ busy, bulk, errorKey, onClose, onSubmit }: { busy: boolean; bulk: boolean; errorKey: string; onClose: () => void; onSubmit: (note: string, andArchive: boolean) => void }) {
  const [note, setNote] = React.useState('');
  const [andArchive, setAndArchive] = React.useState(false);
  const { clear } = React.useContext(ActionFailureContext);
  const dismiss = () => {
    if (busy) return;
    clear(errorKey);
    onClose();
  };
  return (
    <Modal dismissible={!busy} onClose={dismiss} title={bulk ? 'Acknowledge selected alerts' : 'Acknowledge alert'}>
      <label>
        Note <span className="field-hint">(optional)</span>
        <textarea
          className="note-input"
          rows={3}
          placeholder="Describe what you found or what action was taken…"
          value={note}
          onChange={(e) => setNote(e.target.value)}
          autoFocus
        />
      </label>
      <label className="checkbox-label">
        <input type="checkbox" checked={andArchive} onChange={(e) => setAndArchive(e.target.checked)} />
        Also archive after acknowledging
      </label>
      <ActionError actionKey={errorKey} />
      <div className="button-row modal-actions">
        <button className="text-button secondary" disabled={busy} onClick={dismiss}>Cancel</button>
        <button className="text-button" disabled={busy} onClick={() => onSubmit(note, andArchive)}>
          {andArchive ? 'Acknowledge & Archive' : 'Acknowledge'}
        </button>
      </div>
    </Modal>
  );
}

