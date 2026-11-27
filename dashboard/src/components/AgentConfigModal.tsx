import React from 'react';
import { Plus, Trash2, X } from 'lucide-react';
import { setAgentCollectorConfig } from '../api';
import { buildCollectorDraft } from '../collectorDraft';
import { ActionError } from './ActionError';
import { ActionButton } from './ActionButton';
import { AgentIdentity } from './AgentIdentity';
import { useDialog } from './Modal';
import { AnomalyInputs, NetworkThresholdInputs, NumberField, ThresholdInputs, ThresholdRow } from './ThresholdInputs';
import { ActionFailureContext, type RunAction } from '../hooks/useRunAction';
import type { AgentCollectorConfigUpdate, CollectorConfig, DashboardAgent, DiskMonitorConfig, GroupRecord, LogMonitorConfig, NetworkInterfaceConfig, ProcessWatchConfig } from '../models';
import { DEFAULT_ANOMALY_CONFIG, DEFAULT_PERCENT_THRESHOLDS } from '../status';

export function AgentConfigModal({
  agent: agentProp,
  busy,
  global,
  groups,
  onClose,
  operator,
  runAction,
}: {
  agent: DashboardAgent;
  busy: boolean;
  global: boolean;
  groups: GroupRecord[];
  onClose: () => void;
  operator: boolean;
  runAction: RunAction;
}) {
  // Snapshot the agent once. The parent polls every 5s and rebuilds `agents` with new object
  // references, which would otherwise re-render the form on every poll and fight the
  // controlled inputs. The parent gives this modal `key={agent.id}`, so opening a different
  // agent's modal mounts a fresh snapshot.
  const [agent] = React.useState(agentProp);
  const [draft, setDraft] = React.useState<AgentCollectorConfigUpdate>(() => buildCollectorDraft(agent));

  const updateConfig = (update: (config: CollectorConfig) => CollectorConfig) => {
    setDraft((current) => ({ ...current, collector_config: update(current.collector_config) }));
  };
  const updatePercent = (collector: 'cpu' | 'memory', field: keyof typeof DEFAULT_PERCENT_THRESHOLDS, value: number) => {
    updateConfig((config) => ({
      ...config,
      [collector]: { ...config[collector], [field]: value },
    }));
  };
  const updateReading = (
    field: 'cpu_readings' | 'memory_readings' | 'disk_readings' | 'network_readings' | 'process_readings',
    value: number,
  ) => {
    updateConfig((config) => ({ ...config, [field]: value }));
  };
  const updateDisk = (index: number, update: (disk: DiskMonitorConfig) => DiskMonitorConfig) => {
    updateConfig((config) => ({
      ...config,
      disks: config.disks.map((disk, currentIndex) => (currentIndex === index ? update(disk) : disk)),
    }));
  };
  const updateNetwork = (index: number, update: (network: NetworkInterfaceConfig) => NetworkInterfaceConfig) => {
    updateConfig((config) => ({
      ...config,
      networks: config.networks.map((network, currentIndex) => (currentIndex === index ? update(network) : network)),
    }));
  };
  const updateProcess = (index: number, update: (process: ProcessWatchConfig) => ProcessWatchConfig) => {
    updateConfig((config) => ({
      ...config,
      processes: config.processes.map((process, currentIndex) => (currentIndex === index ? update(process) : process)),
    }));
  };
  const updateLog = (index: number, update: (log: LogMonitorConfig) => LogMonitorConfig) => {
    updateConfig((config) => ({
      ...config,
      logs: (config.logs ?? []).map((log, currentIndex) => (currentIndex === index ? update(log) : log)),
    }));
  };

  // Closing is blocked while saving (the failure would have nowhere to show) and drops the old failure.
  const { clear } = React.useContext(ActionFailureContext);
  const dismiss = () => {
    if (busy) return;
    clear(`${agent.id}:collector_config`);
    onClose();
  };

  const dialogRef = React.useRef<HTMLFormElement>(null);
  const onKeyDown = useDialog(dialogRef, !busy, dismiss);

  return (
    <div className="modal-backdrop" role="presentation">
      <form
        aria-label={`Configure ${agent.name}`}
        aria-modal="true"
        className="config-modal"
        onKeyDown={onKeyDown}
        ref={dialogRef}
        role="dialog"
        onSubmit={(event) => {
          event.preventDefault();
          void runAction(`${agent.id}:collector_config`, () => setAgentCollectorConfig(agent.id, draft), { inline: true }).then((saved) => {
            if (saved) onClose();
          });
        }}
      >
        <div className="modal-header">
          <AgentIdentity id={agent.id} name={agent.name} />
          <button className="icon-button" type="button" disabled={busy} onClick={dismiss} aria-label="Close configuration">
            <X size={14} />
          </button>
        </div>

        <div className="config-grid">
          <section>
            <h2>Access</h2>
            <div className="form-grid">
              <label title="Controls which group-scoped users can see alerts, metrics, maintenance, and views for this agent.">
                Group
                <select
                  disabled={!global}
                  value={String(draft.group_ids?.[0] ?? 0)}
                  onChange={(event) => setDraft((current) => ({ ...current, group_ids: Number(event.target.value) > 0 ? [Number(event.target.value)] : [] }))}
                >
                  <option value={0}>No group</option>
                  {groups.map((group) => (
                    <option key={group.group_id} value={group.group_id}>{group.name}</option>
                  ))}
                </select>
              </label>
            </div>
          </section>

          <section>
            <h2>Collection</h2>
            <div className="form-grid">
              <NumberField help="How often collectors such as disk, CPU, memory, network, process, and logs run." label="Collection interval seconds" min={1} value={draft.collection_interval} onChange={(value) => setDraft((current) => ({ ...current, collection_interval: value }))} />
              <NumberField help="How often the agent sends an online heartbeat independent of collector sampling." label="Heartbeat seconds" min={1} max={60} value={draft.heartbeat_interval} onChange={(value) => setDraft((current) => ({ ...current, heartbeat_interval: value }))} />
              <NumberField help="Maximum number of top processes to include in each metric snapshot." label="Process sample limit" min={1} value={draft.process_limit} onChange={(value) => setDraft((current) => ({ ...current, process_limit: value }))} />
              <NumberField help="Consecutive CPU samples required before status changes." label="CPU readings" min={1} value={draft.collector_config.cpu_readings} onChange={(value) => updateReading('cpu_readings', value)} />
              <NumberField help="Consecutive memory samples required before status changes." label="Memory readings" min={1} value={draft.collector_config.memory_readings} onChange={(value) => updateReading('memory_readings', value)} />
              <NumberField help="Consecutive disk samples required before status changes." label="Disk readings" min={1} value={draft.collector_config.disk_readings} onChange={(value) => updateReading('disk_readings', value)} />
              <NumberField help="Consecutive network samples required before status changes." label="Network readings" min={1} value={draft.collector_config.network_readings} onChange={(value) => updateReading('network_readings', value)} />
              <NumberField help="Consecutive process-watch samples required before status changes." label="Process readings" min={1} value={draft.collector_config.process_readings} onChange={(value) => updateReading('process_readings', value)} />
              <NumberField help="Raises stale-data status when a metric value does not change for this many seconds. Zero disables it." label="Stale metric after (seconds, 0=off)" min={0} value={draft.collector_config.stale_after_seconds ?? 0} onChange={(value) => updateConfig((config) => ({ ...config, stale_after_seconds: value }))} />
            </div>
          </section>

          <section className="wide-section">
            <h2>Runbook</h2>
            <textarea
              aria-label="Runbook (Markdown)"
              className="runbook-editor"
              disabled={!operator}
              rows={10}
              title="Markdown instructions for this host. Include alarm-specific steps and links to external pages as needed."
              value={draft.runbook_markdown ?? ''}
              onChange={(event) => setDraft((current) => ({ ...current, runbook_markdown: event.target.value }))}
            />
          </section>

          <section>
            <h2>CPU & Memory</h2>
            <ThresholdRow label="CPU %" thresholds={draft.collector_config.cpu} onChange={(field, value) => updatePercent('cpu', field, value)} />
            <AnomalyInputs
              anomaly={draft.collector_config.cpu_anomaly ?? DEFAULT_ANOMALY_CONFIG}
              onChange={(field, value) => updateConfig((config) => ({ ...config, cpu_anomaly: { ...(config.cpu_anomaly ?? DEFAULT_ANOMALY_CONFIG), [field]: value } }))}
            />
            <ThresholdRow label="Memory %" thresholds={draft.collector_config.memory} onChange={(field, value) => updatePercent('memory', field, value)} />
            <AnomalyInputs
              anomaly={draft.collector_config.memory_anomaly ?? DEFAULT_ANOMALY_CONFIG}
              onChange={(field, value) => updateConfig((config) => ({ ...config, memory_anomaly: { ...(config.memory_anomaly ?? DEFAULT_ANOMALY_CONFIG), [field]: value } }))}
            />
          </section>

          <section className="wide-section">
            <h2>Fixed Disks</h2>
            <div className="config-list">
              {draft.collector_config.disks.map((disk, index) => (
                <div className="config-row" key={disk.mount_point}>
                  <label className="toggle-line" title="Enables or disables monitoring for this disk without removing its saved thresholds.">
                    <input type="checkbox" checked={disk.enabled} onChange={(event) => updateDisk(index, (current) => ({ ...current, enabled: event.target.checked }))} />
                    <span>{disk.device ? `${disk.mount_point} (${disk.device})` : disk.mount_point}</span>
                  </label>
                  <ThresholdInputs
                    thresholds={disk.thresholds}
                    onChange={(field, value) => updateDisk(index, (current) => ({ ...current, thresholds: { ...current.thresholds, [field]: value } }))}
                  />
                  <AnomalyInputs
                    anomaly={disk.anomaly ?? DEFAULT_ANOMALY_CONFIG}
                    onChange={(field, value) => updateDisk(index, (current) => ({ ...current, anomaly: { ...(current.anomaly ?? DEFAULT_ANOMALY_CONFIG), [field]: value } }))}
                  />
                </div>
              ))}
              {draft.collector_config.disks.length === 0 ? <div className="empty-config">No fixed disks reported yet.</div> : null}
            </div>
          </section>

          <section className="wide-section">
            <h2>Network Interfaces</h2>
            <div className="config-list">
              {draft.collector_config.networks.map((network, index) => (
                <div className="config-row" key={network.interface_name}>
                  <label className="toggle-line" title="Enables or disables monitoring for this network interface without removing its saved thresholds.">
                    <input type="checkbox" checked={network.enabled} onChange={(event) => updateNetwork(index, (current) => ({ ...current, enabled: event.target.checked }))} />
                    <span>{network.interface_name}</span>
                  </label>
                  <NetworkThresholdInputs
                    thresholds={network.thresholds}
                    onChange={(field, value) => updateNetwork(index, (current) => ({ ...current, thresholds: { ...current.thresholds, [field]: value } }))}
                  />
                  <AnomalyInputs
                    anomaly={network.anomaly ?? DEFAULT_ANOMALY_CONFIG}
                    onChange={(field, value) => updateNetwork(index, (current) => ({ ...current, anomaly: { ...(current.anomaly ?? DEFAULT_ANOMALY_CONFIG), [field]: value } }))}
                  />
                </div>
              ))}
              {draft.collector_config.networks.length === 0 ? <div className="empty-config">No network interfaces reported yet.</div> : null}
            </div>
          </section>

          <section className="wide-section">
            <div className="section-title-row">
              <h2>Process Watches</h2>
              <ActionButton
                busy={false}
                icon={<Plus size={14} />}
                label="Process"
                onClick={() =>
                  updateConfig((config) => ({
                    ...config,
                    processes: [...config.processes, { name: '', expected_count: 1, enabled: true }],
                  }))
                }
              />
            </div>
            <div className="config-list">
              {draft.collector_config.processes.map((process, index) => (
                // NOTE: do not include `process.name` in the key â€” the name
                // is being edited via the input below, so changing it would
                // change the key, forcing React to unmount/remount the row
                // and dropping the input's focus on every keystroke.
                <div className="process-row" key={`process:${index}`}>
                  <label className="toggle-line" title="Enables or disables this process watch without deleting it.">
                    <input type="checkbox" checked={process.enabled} onChange={(event) => updateProcess(index, (current) => ({ ...current, enabled: event.target.checked }))} />
                    <span>Enabled</span>
                  </label>
                  <input aria-label="Process name" title="Exact executable or process name expected to be running on this agent." value={process.name} placeholder="exact executable name" onChange={(event) => updateProcess(index, (current) => ({ ...current, name: event.target.value }))} />
                  <NumberField help="Expected number of matching processes. Alerts are raised when fewer are running." label="Expected" min={1} value={process.expected_count} onChange={(value) => updateProcess(index, (current) => ({ ...current, expected_count: value }))} />
                  <ActionButton busy={false} danger icon={<Trash2 size={14} />} label="Remove" onClick={() => updateConfig((config) => ({ ...config, processes: config.processes.filter((_, currentIndex) => currentIndex !== index) }))} />
                </div>
              ))}
              {draft.collector_config.processes.length === 0 ? <div className="empty-config">No process watches configured.</div> : null}
            </div>
          </section>

          <section className="wide-section">
            <div className="section-title-row">
              <h2>Log Watches</h2>
              <ActionButton
                busy={false}
                icon={<Plus size={14} />}
                label="Log Watch"
                onClick={() =>
                  updateConfig((config) => ({
                    ...config,
                    logs: [...(config.logs ?? []), { path: '', pattern: '', indicator_name: '', severity: 'red', enabled: true }],
                  }))
                }
              />
            </div>
            <div className="config-list">
              {(draft.collector_config.logs ?? []).map((log, index) => (
                <div className="process-row" key={`log:${index}`}>
                  <label className="toggle-line" title="Enables or disables this log watch without deleting it.">
                    <input type="checkbox" checked={log.enabled} onChange={(event) => updateLog(index, (current) => ({ ...current, enabled: event.target.checked }))} />
                    <span>Enabled</span>
                  </label>
                  <input aria-label="Log file path" title="Absolute path to the log file the agent should scan." value={log.path} placeholder="/var/log/syslog" onChange={(event) => updateLog(index, (current) => ({ ...current, path: event.target.value }))} />
                  <input aria-label="Log match pattern" title="Regular expression pattern that identifies matching log lines." value={log.pattern} placeholder="regex pattern" onChange={(event) => updateLog(index, (current) => ({ ...current, pattern: event.target.value }))} />
                  <input aria-label="Log indicator name" title="Dashboard indicator name to use when this log pattern matches." value={log.indicator_name} placeholder="indicator name" onChange={(event) => updateLog(index, (current) => ({ ...current, indicator_name: event.target.value }))} />
                  <select aria-label="Log severity" title="Alert severity raised when this log pattern matches." value={log.severity} onChange={(event) => updateLog(index, (current) => ({ ...current, severity: event.target.value as LogMonitorConfig['severity'] }))}>
                    <option value="yellow">Warning</option>
                    <option value="amber">Degraded</option>
                    <option value="red">Critical</option>
                  </select>
                  <ActionButton busy={false} danger icon={<Trash2 size={14} />} label="Remove" onClick={() => updateConfig((config) => ({ ...config, logs: (config.logs ?? []).filter((_, currentIndex) => currentIndex !== index) }))} />
                </div>
              ))}
              {(draft.collector_config.logs ?? []).length === 0 ? <div className="empty-config">No log watches configured.</div> : null}
            </div>
          </section>
        </div>

        <ActionError actionKey={`${agent.id}:collector_config`} />
        <div className="modal-actions">
          <button className="text-button" type="button" disabled={busy} onClick={dismiss}>Cancel</button>
          <button className="text-button" type="submit" disabled={!operator || busy}>{busy ? 'Saving…' : 'Save'}</button>
        </div>
      </form>
    </div>
  );
}

