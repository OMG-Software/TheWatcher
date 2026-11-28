import type {
  AgentRecord,
  AlertRecord,
  AgentThresholds,
  AnomalyConfig,
  ComponentHealth,
  CollectorConfig,
  DashboardAgent,
  GroupRecord,
  HealthColor,
  MetricsSnapshot,
  NetworkMetrics,
  NetworkThresholds,
  PercentThresholds,
  SystemMetrics,
} from './models';
import { colorLabel } from './uiLogic';

export type SummaryKey = 'green' | 'yellow' | 'amber' | 'red' | 'blue' | 'offline';
export type OverviewGroupFilter = 'all' | 'ungrouped' | string;

export interface OverviewAgentGroup {
  id: string;
  name: string;
  agents: DashboardAgent[];
}

export interface DisplayAlert extends AlertRecord {
  agentName: string;
  agentId: string;
}

const COLOR_RANK: Record<HealthColor, number> = {
  green: 0,
  grey: 1,
  yellow: 2,
  amber: 3,
  red: 4,
  blue: 5,
};

export const DEFAULT_THRESHOLDS: AgentThresholds = {
  cpu: { warning_pct_of_avg: 125, degraded_pct_of_avg: 150, critical_pct_of_avg: 200 },
  memory: { warning_pct_of_avg: 125, degraded_pct_of_avg: 150, critical_pct_of_avg: 200 },
  disk: { warning_pct_of_avg: 125, degraded_pct_of_avg: 150, critical_pct_of_avg: 200 },
  network: { warning_pct_of_avg: 125, degraded_pct_of_avg: 150, critical_pct_of_avg: 200 },
};

export const DEFAULT_ANOMALY_CONFIG: AnomalyConfig = {
  multiplier: 0,
  baseline_window_hours: 24,
};

export const DEFAULT_PERCENT_THRESHOLDS: PercentThresholds = {
  warning_percent: 80,
  degraded_percent: 90,
  critical_percent: 95,
};

export const DEFAULT_NETWORK_THRESHOLDS: NetworkThresholds = {
  warning_mbps: 100,
  degraded_mbps: 200,
  critical_mbps: 300,
};

export const DEFAULT_COLLECTOR_CONFIG: CollectorConfig = {
  cpu: DEFAULT_PERCENT_THRESHOLDS,
  memory: DEFAULT_PERCENT_THRESHOLDS,
  cpu_readings: 1,
  memory_readings: 1,
  disk_readings: 1,
  network_readings: 1,
  process_readings: 3,
  disks: [],
  networks: [],
  processes: [],
  cpu_anomaly: DEFAULT_ANOMALY_CONFIG,
  memory_anomaly: DEFAULT_ANOMALY_CONFIG,
  stale_after_seconds: 0,
  logs: [],
};

export function collectorConfigWithDefaults(config?: CollectorConfig): CollectorConfig {
  return {
    ...DEFAULT_COLLECTOR_CONFIG,
    ...(config ?? {}),
    cpu: { ...DEFAULT_PERCENT_THRESHOLDS, ...(config?.cpu ?? {}) },
    memory: { ...DEFAULT_PERCENT_THRESHOLDS, ...(config?.memory ?? {}) },
    disks: config?.disks ?? [],
    networks: config?.networks ?? [],
    processes: config?.processes ?? [],
    cpu_anomaly: { ...DEFAULT_ANOMALY_CONFIG, ...(config?.cpu_anomaly ?? {}) },
    memory_anomaly: { ...DEFAULT_ANOMALY_CONFIG, ...(config?.memory_anomaly ?? {}) },
    stale_after_seconds: config?.stale_after_seconds ?? 0,
    logs: config?.logs ?? [],
  };
}

/**
 * The worst of several statuses. Grey means "no status" (not monitored, or not evaluated
 * yet), so it never outranks a real status: grey wins only when nothing else is left.
 * An empty list is green.
 */
export function worstColor(colors: HealthColor[]): HealthColor {
  if (colors.length === 0) return 'green';
  const known = colors.filter((color) => color !== 'grey');
  if (known.length === 0) return 'grey';
  return known.reduce((worst, color) => (COLOR_RANK[color] > COLOR_RANK[worst] ? color : worst), 'green');
}

/**
 * One status per host, from the component statuses the server reported.
 * - Maintenance is blue; an offline host is grey ("Offline" in the UI), whatever its last metrics said.
 * - Components with no status (nothing monitored, or not evaluated yet) are ignored, so they
 *   cannot hold a healthy host in Warning. A host with no status at all is grey.
 */
export function hostStatus(agent: Pick<DashboardAgent, 'maintenance' | 'connected' | 'components'>): HealthColor {
  if (agent.maintenance) return 'blue';
  if (!agent.connected) return 'grey';
  // Heartbeat only says the host is connected, which is already known here; it must not make a
  // host with no evaluated indicator look healthy.
  return worstColor(agent.components.filter((component) => component.key !== 'heartbeat').map((component) => component.color));
}

/** The label for a host row: "Offline" for a disconnected host, otherwise the status name. */
export function hostStatusLabel(agent: Pick<DashboardAgent, 'maintenance' | 'connected' | 'status'>): string {
  if (!agent.maintenance && !agent.connected) return 'Offline';
  return colorLabel(agent.status);
}

export function formatBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let size = bytes;
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }
  return `${size.toFixed(size >= 10 || unit === 0 ? 0 : 1)} ${units[unit]}`;
}

export function formatDuration(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds <= 0) return 'unknown';
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days > 0) return `${days}d ${hours}h ${minutes}m`;
  if (hours > 0) return `${hours}h ${minutes}m`;
  return `${minutes}m`;
}

// Process uptime under this many seconds is treated as an alarm condition
// (the agent has likely just restarted).
export const UPTIME_ALARM_SECONDS = 3600;

export function isUptimeAlarm(uptimeSeconds: number): boolean {
  return Number.isFinite(uptimeSeconds) && uptimeSeconds > 0 && uptimeSeconds < UPTIME_ALARM_SECONDS;
}

function maxPercent(values: number[]): number {
  const finite = values.filter(Number.isFinite);
  return finite.length === 0 ? Number.NaN : Math.max(...finite);
}

function diskLabel(mountPoint: string, device: string): string {
  return device ? `${mountPoint} (${device})` : mountPoint;
}

function networkMbps(network: NetworkMetrics): number {
  return ((network.bytes_recv_per_sec + network.bytes_sent_per_sec) * 8) / 1_000_000;
}

function maintenanceComponents(): ComponentHealth[] {
  return ['cpu', 'memory', 'disk', 'network', 'processes', 'heartbeat'].map((key) => ({
    key: key as ComponentHealth['key'],
    label:
      key === 'processes'
          ? 'Proc'
          : key === 'heartbeat'
            ? 'Heartbeat'
            : key[0].toUpperCase() + key.slice(1),
    color: 'blue' as HealthColor,
    value: 'maintenance',
    detail: 'Maintenance mode',
  }));
}

function heartbeatComponent(agent: AgentRecord): ComponentHealth {
  if (agent.maintenance) {
    return {
      key: 'heartbeat',
      label: 'Heartbeat',
      color: 'blue',
      value: 'maintenance',
      detail: 'Maintenance mode',
    };
  }
  return {
    key: 'heartbeat',
    label: 'Heartbeat',
    color: agent.connected ? 'green' : 'grey',
    value: agent.connected ? 'online' : 'offline',
    detail: agent.last_seen > 0 ? `Last seen ${new Date(agent.last_seen).toLocaleString()}` : 'No heartbeat seen',
  };
}

function noDataComponents(agent: AgentRecord): ComponentHealth[] {
  return [
    { key: 'cpu', label: 'CPU', color: 'grey', value: 'no data', detail: 'Awaiting metrics' },
    { key: 'memory', label: 'Memory', color: 'grey', value: 'no data', detail: 'Awaiting metrics' },
    { key: 'disk', label: 'Disk', color: 'grey', value: 'no data', detail: 'Awaiting metrics' },
    { key: 'network', label: 'Network', color: 'grey', value: 'no data', detail: 'Awaiting metrics' },
    { key: 'processes', label: 'Proc', color: 'grey', value: 'no data', detail: 'Awaiting metrics' },
    heartbeatComponent(agent),
  ];
}

const SERVER_STATUSES: readonly HealthColor[] = ['green', 'grey', 'yellow', 'amber', 'red', 'blue'];

/**
 * What the status engine last decided for one indicator. The server only raises an alert after
 * the configured number of consecutive readings, so this is the status to show, not the latest
 * sample classified here. Grey when the server has not reported the indicator yet.
 */
function serverStatus(agent: AgentRecord, indicator: string): HealthColor {
  const status = agent.indicator_status?.[indicator] as HealthColor | undefined;
  return status && SERVER_STATUSES.includes(status) ? status : 'grey';
}

const NOT_EVALUATED = 'Awaiting server evaluation';

function metricComponents(agent: AgentRecord, metrics?: SystemMetrics): ComponentHealth[] {
  if (agent.maintenance) return maintenanceComponents();
  if (!metrics) return noDataComponents(agent);

  const config = collectorConfigWithDefaults(agent.collector_config);
  // Which disks, interfaces and processes the server evaluates follows the agent's collector
  // config; only the statuses come from the server.
  const diskConfigByMount = new Map(config.disks.map((disk) => [disk.mount_point, disk]));
  const monitoredDisks =
    config.disks.length === 0
      ? metrics.disks
      : metrics.disks.filter((disk) => diskConfigByMount.get(disk.mount_point)?.enabled);
  const diskUsage = maxPercent(monitoredDisks.map((disk) => disk.usage_percent));
  const hottestDisk = monitoredDisks.reduce(
    (current, disk) => (disk.usage_percent > (current?.usage_percent ?? Number.NEGATIVE_INFINITY) ? disk : current),
    undefined as (typeof monitoredDisks)[number] | undefined,
  );
  const networkConfigByName = new Map(config.networks.map((network) => [network.interface_name, network]));
  const monitoredNetworks =
    config.networks.length === 0
      ? metrics.networks.filter((network) => network.interface_name !== 'lo')
      : metrics.networks.filter((network) => networkConfigByName.get(network.interface_name)?.enabled);
  const networkErrors = monitoredNetworks.reduce(
    (sum, net) => sum + net.errors_in + net.errors_out + net.drops_in + net.drops_out,
    0,
  );
  const networkTraffic = monitoredNetworks.reduce((sum, net) => sum + networkMbps(net), 0);
  const topCpu = maxPercent(metrics.top_processes.map((process) => process.cpu_percent));
  const enabledWatches = config.processes.filter((process) => process.enabled && process.name.trim().length > 0);
  const missingProcesses = enabledWatches.filter((watch) => {
    const count = metrics.top_processes.filter((process) => process.name === watch.name).length;
    return count < watch.expected_count;
  });
  const diskColor = monitoredDisks.length === 0 ? 'grey' : worstColor(monitoredDisks.map((disk) => serverStatus(agent, `disk:${disk.mount_point}`)));
  const networkColor = monitoredNetworks.length === 0 ? 'grey' : worstColor(monitoredNetworks.map((net) => serverStatus(agent, `network:${net.interface_name}`)));
  // Without watches the server evaluates no process indicator, so there is nothing to show.
  const processColor = enabledWatches.length === 0 ? 'grey' : worstColor(enabledWatches.map((watch) => serverStatus(agent, `process:${watch.name}`)));
  const cpuColor = serverStatus(agent, 'cpu');
  const memoryColor = serverStatus(agent, 'memory');
  const pendingDetail = (color: HealthColor, detail: string) => (color === 'grey' ? `${detail} (${NOT_EVALUATED.toLowerCase()})` : detail);

  return [
    {
      key: 'cpu',
      label: 'CPU',
      color: cpuColor,
      value: `${metrics.cpu.usage_percent.toFixed(0)}%`,
      detail: pendingDetail(cpuColor, `${metrics.cpu.num_logical_cores} logical cores`),
      percent: metrics.cpu.usage_percent,
    },
    {
      key: 'memory',
      label: 'Memory',
      color: memoryColor,
      value: `${metrics.memory.usage_percent.toFixed(0)}%`,
      detail: pendingDetail(memoryColor, `${formatBytes(metrics.memory.used_bytes)} / ${formatBytes(metrics.memory.total_bytes)}`),
      percent: metrics.memory.usage_percent,
    },
    {
      key: 'disk',
      label: 'Disk',
      color: diskColor,
      value: monitoredDisks.length === 0 ? 'none' : `${diskUsage.toFixed(0)}%`,
      detail: hottestDisk ? pendingDetail(diskColor, diskLabel(hottestDisk.mount_point, hottestDisk.device)) : 'No monitored fixed disks',
      percent: Number.isFinite(diskUsage) ? diskUsage : undefined,
    },
    {
      key: 'network',
      label: 'Network',
      color: networkColor,
      value: monitoredNetworks.length === 0 ? 'none' : `${networkTraffic.toFixed(0)} Mb/s`,
      detail:
        monitoredNetworks.length === 0
          ? 'No monitored interfaces'
          : pendingDetail(networkColor, networkErrors > 0 ? `${networkErrors} errors/drops` : `${monitoredNetworks.length} interface(s)`),
    },
    {
      key: 'processes',
      label: 'Proc',
      color: processColor,
      value:
        enabledWatches.length === 0
          ? metrics.top_processes.length === 0
            ? 'none'
            : `${topCpu.toFixed(0)}% top`
          : `${enabledWatches.length - missingProcesses.length}/${enabledWatches.length}`,
      detail:
        missingProcesses.length > 0
          ? `Missing ${missingProcesses.map((process) => process.name).join(', ')}`
          : enabledWatches.length > 0
            ? pendingDetail(processColor, `${enabledWatches.length} watched process(es)`)
            : 'No watched processes',
      percent: metrics.top_processes.length === 0 ? undefined : topCpu,
    },
    heartbeatComponent(agent),
  ];
}

export function primaryIpAddress(metrics?: SystemMetrics): string {
  const candidates = metrics?.networks.filter((network) => {
    const ip = network.ip_address?.trim();
    return ip && ip !== '127.0.0.1' && ip !== '::1' && network.interface_name !== 'lo';
  }) ?? [];
  return candidates[0]?.ip_address ?? '';
}

export function toDashboardAgents(
  agents: AgentRecord[],
  snapshots: MetricsSnapshot[],
  alerts: AlertRecord[] = [],
): DashboardAgent[] {
  const latest = new Map(snapshots.map((snapshot) => [snapshot.agent_id, snapshot]));
  const unacked = new Set(alerts.filter((alert) => alert.acknowledged_at === 0).map((alert) => alert.agent_id));

  return agents
    .map((agent) => {
      const snapshot = latest.get(agent.agent_id);
      const metrics = snapshot?.metrics;
      const components = metricComponents(agent, metrics);
      const row: DashboardAgent = {
        id: agent.agent_id,
        name: metrics?.hostname || agent.hostname || agent.agent_id,
        platform: metrics?.platform || agent.platform || 'unknown',
        approved: agent.approved,
        rejected: agent.rejected,
        connected: agent.connected,
        maintenance: agent.maintenance,
        maintenanceReason: agent.maintenance_reason ?? '',
        maintenanceUntil: agent.maintenance_until ?? 0,
        collectionInterval: agent.collection_interval,
        heartbeatInterval: agent.heartbeat_interval ?? 5,
        processLimit: agent.process_limit,
        thresholds: agent.thresholds ?? DEFAULT_THRESHOLDS,
        collectorConfig: collectorConfigWithDefaults(agent.collector_config),
        lastSeen: agent.last_seen,
        uptimeSeconds: metrics?.uptime_seconds ?? 0,
        uptime: metrics ? formatDuration(metrics.uptime_seconds) : 'unknown',
        group: agent.maintenance ? 'Maintenance Agents' : 'Approved Agents',
        groupIds: agent.group_ids ?? [],
        status: 'green',
        alertColor: unacked.has(agent.agent_id) ? 'red' : 'green',
        components,
        metrics,
        ipAddress: primaryIpAddress(metrics),
        description: agent.description ?? '',
        runbookMarkdown: agent.runbook_markdown ?? '',
      };
      row.status = hostStatus(row);
      return row;
    })
    .sort((a, b) => a.group.localeCompare(b.group) || a.name.localeCompare(b.name));
}

export function agentStatus(agent: DashboardAgent): HealthColor {
  return agent.status;
}

export function groupOverviewAgents(
  agents: DashboardAgent[],
  groups: GroupRecord[],
  filter: OverviewGroupFilter,
): OverviewAgentGroup[] {
  const groupNames = new Map(groups.map((group) => [group.group_id, group.name]));
  const buckets = new Map<string, OverviewAgentGroup>();

  const addToBucket = (id: string, name: string, agent: DashboardAgent) => {
    const bucket = buckets.get(id) ?? { id, name, agents: [] };
    bucket.agents.push(agent);
    buckets.set(id, bucket);
  };

  agents.forEach((agent) => {
    const groupIds = agent.groupIds.filter((id) => groupNames.has(id));
    if (filter === 'ungrouped') {
      if (groupIds.length === 0) addToBucket('ungrouped', 'Ungrouped', agent);
      return;
    }

    if (filter !== 'all') {
      const selected = Number.parseInt(filter, 10);
      if (Number.isFinite(selected) && groupIds.includes(selected)) {
        addToBucket(String(selected), groupNames.get(selected) ?? `Group ${selected}`, agent);
      }
      return;
    }

    if (groupIds.length === 0) {
      addToBucket('ungrouped', 'Ungrouped', agent);
      return;
    }

    groupIds.forEach((groupId) => {
      addToBucket(String(groupId), groupNames.get(groupId) ?? `Group ${groupId}`, agent);
    });
  });

  return [...buckets.values()]
    .map((group) => ({
      ...group,
      agents: [...group.agents].sort((a, b) => a.name.localeCompare(b.name)),
    }))
    .sort((a, b) => {
      if (a.id === 'ungrouped') return 1;
      if (b.id === 'ungrouped') return -1;
      return a.name.localeCompare(b.name);
    });
}

export function summaryCounts(agents: DashboardAgent[]): Record<SummaryKey, number> {
  const counts: Record<SummaryKey, number> = { green: 0, yellow: 0, amber: 0, red: 0, blue: 0, offline: 0 };
  agents.forEach((agent) => {
    if (!agent.connected && !agent.maintenance) {
      counts.offline += 1;
      return;
    }
    const status = agentStatus(agent);
    // A host with nothing evaluated yet is not known healthy, so it counts as Warning.
    if (status === 'grey') counts.yellow += 1;
    else counts[status as SummaryKey] += 1;
  });
  return counts;
}

export function toDisplayAlerts(alerts: AlertRecord[], agents: DashboardAgent[]): DisplayAlert[] {
  const byId = new Map(agents.map((agent) => [agent.id, agent]));
  return alerts.map((alert) => {
    const agent = byId.get(alert.agent_id);
    return {
      ...alert,
      agentName: agent?.name ?? alert.agent_id,
      agentId: alert.agent_id,
    };
  });
}
