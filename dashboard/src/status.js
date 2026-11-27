const COLOR_RANK = {
    green: 0,
    grey: 1,
    yellow: 2,
    amber: 3,
    red: 4,
    blue: 5,
};
export const DEFAULT_THRESHOLDS = {
    cpu: { warning_pct_of_avg: 125, degraded_pct_of_avg: 150, critical_pct_of_avg: 200 },
    memory: { warning_pct_of_avg: 125, degraded_pct_of_avg: 150, critical_pct_of_avg: 200 },
    disk: { warning_pct_of_avg: 125, degraded_pct_of_avg: 150, critical_pct_of_avg: 200 },
    network: { warning_pct_of_avg: 125, degraded_pct_of_avg: 150, critical_pct_of_avg: 200 },
};
export const DEFAULT_ANOMALY_CONFIG = {
    multiplier: 0,
    baseline_window_hours: 24,
};
export const DEFAULT_PERCENT_THRESHOLDS = {
    warning_percent: 80,
    degraded_percent: 90,
    critical_percent: 95,
};
export const DEFAULT_NETWORK_THRESHOLDS = {
    warning_mbps: 100,
    degraded_mbps: 200,
    critical_mbps: 300,
};
export const DEFAULT_COLLECTOR_CONFIG = {
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
export function collectorConfigWithDefaults(config) {
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
export function classifyPercent(value, thresholds = DEFAULT_PERCENT_THRESHOLDS) {
    if (!Number.isFinite(value))
        return 'grey';
    if (value >= thresholds.critical_percent)
        return 'red';
    if (value >= thresholds.degraded_percent)
        return 'amber';
    if (value >= thresholds.warning_percent)
        return 'yellow';
    return 'green';
}
export function classifyNetworkMbps(value, thresholds = DEFAULT_NETWORK_THRESHOLDS) {
    if (!Number.isFinite(value))
        return 'grey';
    if (value >= thresholds.critical_mbps)
        return 'red';
    if (value >= thresholds.degraded_mbps)
        return 'amber';
    if (value >= thresholds.warning_mbps)
        return 'yellow';
    return 'green';
}
export function worstColor(colors) {
    return colors.reduce((worst, color) => (COLOR_RANK[color] > COLOR_RANK[worst] ? color : worst), 'green');
}
export function hostStatus(agent) {
    if (agent.maintenance)
        return 'blue';
    const componentWorst = worstColor(agent.components.map((component) => component.color));
    return componentWorst === 'grey' ? 'yellow' : componentWorst;
}
export function formatBytes(bytes) {
    if (!Number.isFinite(bytes) || bytes <= 0)
        return '0 B';
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    let size = bytes;
    let unit = 0;
    while (size >= 1024 && unit < units.length - 1) {
        size /= 1024;
        unit += 1;
    }
    return `${size.toFixed(size >= 10 || unit === 0 ? 0 : 1)} ${units[unit]}`;
}
export function formatDuration(seconds) {
    if (!Number.isFinite(seconds) || seconds <= 0)
        return 'unknown';
    const days = Math.floor(seconds / 86400);
    const hours = Math.floor((seconds % 86400) / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    if (days > 0)
        return `${days}d ${hours}h ${minutes}m`;
    if (hours > 0)
        return `${hours}h ${minutes}m`;
    return `${minutes}m`;
}
// Process uptime under this many seconds is treated as an alarm condition
// (the agent has likely just restarted).
export const UPTIME_ALARM_SECONDS = 3600;
export function isUptimeAlarm(uptimeSeconds) {
    return Number.isFinite(uptimeSeconds) && uptimeSeconds > 0 && uptimeSeconds < UPTIME_ALARM_SECONDS;
}
function maxPercent(values) {
    const finite = values.filter(Number.isFinite);
    return finite.length === 0 ? Number.NaN : Math.max(...finite);
}
function diskLabel(mountPoint, device) {
    return device ? `${mountPoint} (${device})` : mountPoint;
}
// The agent reports errors and drops as the change since its previous sample,
// so this is a per-interval count: a few stray drops are normal, a burst is not.
export const NETWORK_ERROR_DELTA_THRESHOLD = 10;
const PSEUDO_INTERFACE_PREFIXES = ['lo', 'pflog', 'pfsync', 'enc', 'ipfw'];
const PSEUDO_INTERFACE_FRAGMENTS = ['loopback', 'pseudo-interface', 'isatap', 'teredo', '6to4'];
// Mirrors the server's default network evaluation: with no explicit network
// config only real, connected interfaces are monitored, so loopback, BSD/Windows
// pseudo devices and down interfaces are skipped.
function isPseudoInterfaceName(interfaceName) {
    const name = interfaceName.toLowerCase();
    if (PSEUDO_INTERFACE_PREFIXES.some((prefix) => name.startsWith(prefix) && /^\d*$/.test(name.slice(prefix.length))))
        return true;
    return PSEUDO_INTERFACE_FRAGMENTS.some((fragment) => name.includes(fragment));
}
function isDefaultMonitoredNetwork(network) {
    return network.is_up && network.is_loopback !== true && !isPseudoInterfaceName(network.interface_name);
}
function networkMbps(network) {
    return ((network.bytes_recv_per_sec + network.bytes_sent_per_sec) * 8) / 1_000_000;
}
function maintenanceComponents() {
    return ['cpu', 'memory', 'disk', 'network', 'processes', 'heartbeat'].map((key) => ({
        key: key,
        label: key === 'processes'
            ? 'Proc'
            : key === 'heartbeat'
                ? 'Heartbeat'
                : key[0].toUpperCase() + key.slice(1),
        color: 'blue',
        value: 'maintenance',
        detail: 'Maintenance mode',
    }));
}
function heartbeatComponent(agent) {
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
function noDataComponents(agent) {
    return [
        { key: 'cpu', label: 'CPU', color: 'grey', value: 'no data', detail: 'Awaiting metrics' },
        { key: 'memory', label: 'Memory', color: 'grey', value: 'no data', detail: 'Awaiting metrics' },
        { key: 'disk', label: 'Disk', color: 'grey', value: 'no data', detail: 'Awaiting metrics' },
        { key: 'network', label: 'Network', color: 'grey', value: 'no data', detail: 'Awaiting metrics' },
        { key: 'processes', label: 'Proc', color: 'grey', value: 'no data', detail: 'Awaiting metrics' },
        heartbeatComponent(agent),
    ];
}
function metricComponents(agent, metrics) {
    if (agent.maintenance)
        return maintenanceComponents();
    if (!metrics)
        return noDataComponents(agent);
    const config = collectorConfigWithDefaults(agent.collector_config);
    const diskConfigByMount = new Map(config.disks.map((disk) => [disk.mount_point, disk]));
    const monitoredDisks = config.disks.length === 0
        ? metrics.disks
        : metrics.disks.filter((disk) => diskConfigByMount.get(disk.mount_point)?.enabled);
    const diskStatuses = monitoredDisks.map((disk) => classifyPercent(disk.usage_percent, diskConfigByMount.get(disk.mount_point)?.thresholds ?? DEFAULT_PERCENT_THRESHOLDS));
    const diskUsage = maxPercent(monitoredDisks.map((disk) => disk.usage_percent));
    const hottestDisk = monitoredDisks.reduce((current, disk) => (disk.usage_percent > (current?.usage_percent ?? Number.NEGATIVE_INFINITY) ? disk : current), undefined);
    const networkConfigByName = new Map(config.networks.map((network) => [network.interface_name, network]));
    const monitoredNetworks = config.networks.length === 0
        ? metrics.networks.filter(isDefaultMonitoredNetwork)
        : metrics.networks.filter((network) => networkConfigByName.get(network.interface_name)?.enabled);
    const networkErrors = monitoredNetworks.reduce((sum, net) => sum + net.errors_in + net.errors_out + net.drops_in + net.drops_out, 0);
    const networkStatuses = monitoredNetworks.map((net) => {
        if (!net.is_up)
            return 'red';
        if (net.errors_in + net.errors_out + net.drops_in + net.drops_out >= NETWORK_ERROR_DELTA_THRESHOLD)
            return 'red';
        return classifyNetworkMbps(networkMbps(net), networkConfigByName.get(net.interface_name)?.thresholds ?? DEFAULT_NETWORK_THRESHOLDS);
    });
    const networkTraffic = monitoredNetworks.reduce((sum, net) => sum + networkMbps(net), 0);
    const topCpu = maxPercent(metrics.top_processes.map((process) => process.cpu_percent));
    const enabledWatches = config.processes.filter((process) => process.enabled && process.name.trim().length > 0);
    const missingProcesses = enabledWatches.filter((watch) => {
        const count = metrics.top_processes.filter((process) => process.name === watch.name).length;
        return count < watch.expected_count;
    });
    const processColor = enabledWatches.length === 0
        ? metrics.top_processes.length === 0
            ? 'grey'
            : 'green'
        : missingProcesses.length > 0
            ? 'red'
            : 'green';
    return [
        {
            key: 'cpu',
            label: 'CPU',
            color: classifyPercent(metrics.cpu.usage_percent, config.cpu),
            value: `${metrics.cpu.usage_percent.toFixed(0)}%`,
            detail: `${metrics.cpu.num_logical_cores} logical cores`,
            percent: metrics.cpu.usage_percent,
        },
        {
            key: 'memory',
            label: 'Memory',
            color: classifyPercent(metrics.memory.usage_percent, config.memory),
            value: `${metrics.memory.usage_percent.toFixed(0)}%`,
            detail: `${formatBytes(metrics.memory.used_bytes)} / ${formatBytes(metrics.memory.total_bytes)}`,
            percent: metrics.memory.usage_percent,
        },
        {
            key: 'disk',
            label: 'Disk',
            color: monitoredDisks.length === 0 ? 'grey' : worstColor(diskStatuses),
            value: monitoredDisks.length === 0 ? 'none' : `${diskUsage.toFixed(0)}%`,
            detail: hottestDisk ? diskLabel(hottestDisk.mount_point, hottestDisk.device) : 'No monitored fixed disks',
            percent: Number.isFinite(diskUsage) ? diskUsage : undefined,
        },
        {
            key: 'network',
            label: 'Network',
            color: monitoredNetworks.length === 0 ? 'grey' : worstColor(networkStatuses),
            value: monitoredNetworks.length === 0 ? 'none' : `${networkTraffic.toFixed(0)} Mb/s`,
            detail: networkErrors > 0 ? `${networkErrors} errors/drops in the last interval` : `${monitoredNetworks.length} interface(s)`,
        },
        {
            key: 'processes',
            label: 'Proc',
            color: processColor,
            value: enabledWatches.length === 0
                ? metrics.top_processes.length === 0
                    ? 'none'
                    : `${topCpu.toFixed(0)}% top`
                : `${enabledWatches.length - missingProcesses.length}/${enabledWatches.length}`,
            detail: missingProcesses.length > 0
                ? `Missing ${missingProcesses.map((process) => process.name).join(', ')}`
                : enabledWatches.length > 0
                    ? `${enabledWatches.length} watched process(es)`
                    : `${metrics.top_processes.length} tracked process(es)`,
            percent: metrics.top_processes.length === 0 ? undefined : topCpu,
        },
        heartbeatComponent(agent),
    ];
}
function isLoopbackNetwork(network) {
    const ip = network.ip_address?.trim();
    return (network.is_loopback === true ||
        isPseudoInterfaceName(network.interface_name) ||
        ip === '::1' ||
        (ip?.startsWith('127.') ?? false));
}
// The address shown for an agent: the first interface that has one and is not
// loopback or a pseudo device (lo, lo0, pflog0, ...). Down interfaces count,
// since their last address is still the best identifier.
export function primaryIpAddress(metrics) {
    const candidates = metrics?.networks.filter((network) => network.ip_address?.trim() && !isLoopbackNetwork(network)) ?? [];
    return candidates[0]?.ip_address ?? '';
}
export function toDashboardAgents(agents, snapshots, alerts = []) {
    const latest = new Map(snapshots.map((snapshot) => [snapshot.agent_id, snapshot]));
    const unacked = new Set(alerts.filter((alert) => alert.acknowledged_at === 0).map((alert) => alert.agent_id));
    return agents
        .map((agent) => {
        const snapshot = latest.get(agent.agent_id);
        const metrics = snapshot?.metrics;
        const components = metricComponents(agent, metrics);
        const row = {
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
export function agentStatus(agent) {
    return agent.status;
}
export function groupOverviewAgents(agents, groups, filter) {
    const groupNames = new Map(groups.map((group) => [group.group_id, group.name]));
    const buckets = new Map();
    const addToBucket = (id, name, agent) => {
        const bucket = buckets.get(id) ?? { id, name, agents: [] };
        bucket.agents.push(agent);
        buckets.set(id, bucket);
    };
    agents.forEach((agent) => {
        const groupIds = agent.groupIds.filter((id) => groupNames.has(id));
        if (filter === 'ungrouped') {
            if (groupIds.length === 0)
                addToBucket('ungrouped', 'Ungrouped', agent);
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
        if (a.id === 'ungrouped')
            return 1;
        if (b.id === 'ungrouped')
            return -1;
        return a.name.localeCompare(b.name);
    });
}
export function summaryCounts(agents) {
    const counts = { green: 0, yellow: 0, amber: 0, red: 0, blue: 0, offline: 0 };
    agents.forEach((agent) => {
        if (!agent.connected && !agent.maintenance) {
            counts.offline += 1;
            return;
        }
        const status = agentStatus(agent);
        if (status === 'grey')
            counts.yellow += 1;
        else
            counts[status] += 1;
    });
    return counts;
}
export function toDisplayAlerts(alerts, agents) {
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
