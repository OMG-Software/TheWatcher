import { X } from 'lucide-react';
import { RunbookMarkdown } from './RunbookMarkdown';

export function RunbookModal({ agentName, markdown, onClose }: { agentName: string; markdown: string; onClose: () => void }) {
  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal-panel runbook-modal" onClick={(event) => event.stopPropagation()}>
        <div className="modal-header">
          <h2>Runbook - {agentName}</h2>
          <button className="icon-button" onClick={onClose} aria-label="Close runbook"><X size={14} /></button>
        </div>
        <div className="runbook-body">
          <RunbookMarkdown markdown={markdown} />
        </div>
      </div>
    </div>
  );
}

