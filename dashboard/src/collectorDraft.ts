import type { AgentCollectorConfigUpdate, DashboardAgent, DiskMonitorConfig, NetworkInterfaceConfig } from './models';
import { DEFAULT_ANOMALY_CONFIG, DEFAULT_NETWORK_THRESHOLDS, DEFAULT_PERCENT_THRESHOLDS, collectorConfigWithDefaults } from './status';

export function buildCollectorDraft(agent: DashboardAgent): AgentCollectorConfigUpdate {
  const config = collectorConfigWithDefaults(agent.collectorConfig);
  const diskByMount = new Map(config.disks.map((disk) => [disk.mount_point, disk]));
  const metricDisks =
    agent.metrics?.disks
      .filter((disk) => !diskByMount.has(disk.mount_point))
      .map<DiskMonitorConfig>((disk) => ({
        mount_point: disk.mount_point,
        device: disk.device,
        enabled: true,
        thresholds: { ...DEFAULT_PERCENT_THRESHOLDS },
        anomaly: { ...DEFAULT_ANOMALY_CONFIG },
      })) ?? [];
  const networkByName = new Map(config.networks.map((network) => [network.interface_name, network]));
  const metricNetworks =
    agent.metrics?.networks
      .filter((network) => network.interface_name !== 'lo' && !networkByName.has(network.interface_name))
      .map<NetworkInterfaceConfig>((network) => ({
        interface_name: network.interface_name,
        enabled: true,
        thresholds: { ...DEFAULT_NETWORK_THRESHOLDS },
        anomaly: { ...DEFAULT_ANOMALY_CONFIG },
      })) ?? [];

  return {
    collection_interval: agent.collectionInterval,
    heartbeat_interval: agent.heartbeatInterval,
    process_limit: agent.processLimit,
    group_ids: agent.groupIds,
    runbook_markdown: agent.runbookMarkdown,
    collector_config: {
      ...config,
      cpu: { ...config.cpu },
      memory: { ...config.memory },
      disks: [
        ...config.disks.map((disk) => ({
          ...disk,
          thresholds: { ...disk.thresholds },
          anomaly: { ...DEFAULT_ANOMALY_CONFIG, ...(disk.anomaly ?? {}) },
        })),
        ...metricDisks,
      ],
      networks: [
        ...config.networks.map((network) => ({
          ...network,
          thresholds: { ...network.thresholds },
          anomaly: { ...DEFAULT_ANOMALY_CONFIG, ...(network.anomaly ?? {}) },
        })),
        ...metricNetworks,
      ],
      processes: config.processes.map((process) => ({ ...process })),
      logs: (config.logs ?? []).map((log) => ({ ...log })),
    },
  };
}

