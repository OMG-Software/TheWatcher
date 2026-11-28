import { Modal } from './Modal';
import { RunbookMarkdown } from './RunbookMarkdown';

export function RunbookModal({ agentName, markdown, onClose }: { agentName: string; markdown: string; onClose: () => void }) {
  return (
    <Modal className="runbook-modal" closeLabel="Close runbook" onClose={onClose} title={`Runbook - ${agentName}`}>
      <div className="runbook-body">
        <RunbookMarkdown markdown={markdown} />
      </div>
    </Modal>
  );
}
