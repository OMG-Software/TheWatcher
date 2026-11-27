import React from 'react';
import { X } from 'lucide-react';

export function AcknowledgeModal({ onClose, onSubmit, bulk }: { onClose: () => void; onSubmit: (note: string, andArchive: boolean) => void; bulk: boolean }) {
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
        <div className="button-row modal-actions">
          <button className="text-button secondary" onClick={onClose}>Cancel</button>
          <button className="text-button" onClick={() => onSubmit(note, andArchive)}>
            {andArchive ? 'Acknowledge & Archive' : 'Acknowledge'}
          </button>
        </div>
      </div>
    </div>
  );
}

