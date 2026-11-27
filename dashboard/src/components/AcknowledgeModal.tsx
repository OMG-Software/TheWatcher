import React from 'react';
import { X } from 'lucide-react';
import { ActionError } from './ActionError';

/** Stays open (with the note) until the parent closes it, so a failed save loses nothing; the parent shows the failure under `errorKey`. */
export function AcknowledgeModal({ busy, bulk, errorKey, onClose, onSubmit }: { busy: boolean; bulk: boolean; errorKey: string; onClose: () => void; onSubmit: (note: string, andArchive: boolean) => void }) {
  const [note, setNote] = React.useState('');
  const [andArchive, setAndArchive] = React.useState(false);
  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal-panel" onClick={(e) => e.stopPropagation()}>
        <div className="modal-header">
          <h2>{bulk ? 'Acknowledge selected alerts' : 'Acknowledge alert'}</h2>
          <button className="icon-button" onClick={onClose}><X size={14} /></button>
        </div>
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
          <button className="text-button secondary" onClick={onClose}>Cancel</button>
          <button className="text-button" disabled={busy} onClick={() => onSubmit(note, andArchive)}>
            {andArchive ? 'Acknowledge & Archive' : 'Acknowledge'}
          </button>
        </div>
      </div>
    </div>
  );
}

