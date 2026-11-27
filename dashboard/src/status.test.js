import { describe, expect, it } from 'vitest';
import { DEFAULT_COLLECTOR_CONFIG, DEFAULT_NETWORK_THRESHOLDS, DEFAULT_PERCENT_THRESHOLDS, DEFAULT_THRESHOLDS, NETWORK_ERROR_DELTA_THRESHOLD, agentStatus, classifyNetworkMbps, classifyPercent, collectorConfigWithDefaults, formatBytes, formatDuration, isUptimeAlarm, groupOverviewAgents, hostStatus, primaryIpAddress, summaryCounts, toDashboardAgents, toDisplayAlerts, worstColor, } from './status';
function agent(overrides = {}) {
    return {
        agent_id: 'agent-1',
        hostname: 'host-1',
        platform: 'linux',
        curve_public_key_z85: '',
        approved: true,
        rejected: false,
        connected: true,
        maintenance: false,
        maintenance_reason: '',
        maintenance_until: 0,
        collection_interval: 30,
        heartbeat_interval: 5,
        process_limit: 25,
        first_seen: 1,
        last_seen: 1,
        description: '',
        runbook_markdown: '',
        ...overrides,
    };
}
describe('GIVEN dashboard health thresholds', () => {
    it('WHEN percentages are classified THEN severity increases at warning, degraded, and critical levels', () => {
        expect(classifyPercent(12)).toBe('green');
        expect(classifyPercent(80)).toBe('yellow');
        expect(classifyPercent(90)).toBe('amber');
        expect(classifyPercent(95)).toBe('red');
    });
});
describe('GIVEN agents with component health', () => {
    it('WHEN summary counts are calculated THEN each agent contributes to its host state', () => {
        const agents = [
            { id: 'a', name: 'a', platform: 'linux', approved: true, rejected: false, connected: true, maintenance: false, maintenanceReason: '', maintenanceUntil: 0, collectionInterval: 30, heartbeatInterval: 5, processLimit: 25, thresholds: DEFAULT_THRESHOLDS, collectorConfig: DEFAULT_COLLECTOR_CONFIG, lastSeen: 0, uptimeSeconds: 60, uptime: '1m', group: 'g', groupIds: [], status: 'green', alertColor: 'green', components: [{ key: 'cpu', label: 'CPU', color: 'green', value: '1%', detail: '' }], ipAddress: '', description: '', runbookMarkdown: '' },
            { id: 'b', name: 'b', platform: 'linux', approved: true, rejected: false, connected: true, maintenance: false, maintenanceReason: '', maintenanceUntil: 0, collectionInterval: 30, heartbeatInterval: 5, processLimit: 25, thresholds: DEFAULT_THRESHOLDS, collectorConfig: DEFAULT_COLLECTOR_CONFIG, lastSeen: 0, uptimeSeconds: 60, uptime: '1m', group: 'g', groupIds: [], status: 'yellow', alertColor: 'green', components: [{ key: 'cpu', label: 'CPU', color: 'yellow', value: '65%', detail: '' }], ipAddress: '', description: '', runbookMarkdown: '' },
            { id: 'c', name: 'c', platform: 'linux', approved: true, rejected: false, connected: true, maintenance: false, maintenanceReason: '', maintenanceUntil: 0, collectionInterval: 30, heartbeatInterval: 5, processLimit: 25, thresholds: DEFAULT_THRESHOLDS, collectorConfig: DEFAULT_COLLECTOR_CONFIG, lastSeen: 0, uptimeSeconds: 60, uptime: '1m', group: 'g', groupIds: [], status: 'red', alertColor: 'red', components: [{ key: 'cpu', label: 'CPU', color: 'red', value: '95%', detail: '' }], ipAddress: '', description: '', runbookMarkdown: '' },
        ];
        expect(agentStatus(agents[2])).toBe('red');
        expect(summaryCounts(agents)).toMatchObject({ green: 1, yellow: 1, red: 1 });
    });
});
describe('GIVEN backend agents and metrics', () => {
    it('WHEN dashboard rows are built THEN missing metrics are represented as no-data state', () => {
        const rows = toDashboardAgents([agent()], []);
        expect(rows).toHaveLength(1);
        expect(rows[0].components.find((component) => component.key === 'cpu')?.color).toBe('grey');
        expect(rows[0].components.at(-1)).toMatchObject({ key: 'heartbeat', color: 'green' });
        expect(rows[0].status).toBe('yellow');
    });
    it('WHEN a disconnected agent row is built THEN heartbeat is the final no-data indicator', () => {
        const rows = toDashboardAgents([agent({ connected: false, last_seen: 0 })], []);
        expect(rows[0].components.at(-1)).toMatchObject({
            key: 'heartbeat',
            label: 'Heartbeat',
            color: 'grey',
            value: 'offline',
        });
    });
    it('WHEN maintenance agents are built THEN all indicators are blue', () => {
        const rows = toDashboardAgents([agent({ agent_id: 'agent-maintenance', maintenance: true })], []);
        expect(rows[0].group).toBe('Maintenance Agents');
        expect(rows[0].maintenance).toBe(true);
        expect(rows[0].components.every((component) => component.color === 'blue')).toBe(true);
        expect(rows[0].components.at(-1)).toMatchObject({ key: 'heartbeat', color: 'blue' });
    });
    it('WHEN collector config is supplied THEN dashboard health uses absolute per-agent thresholds and watches', () => {
        const metrics = {
            agent_id: 'agent-1',
            timestamp_ms: 1000,
            metrics: {
                cpu: { usage_percent: 55, num_logical_cores: 8, load_avg_1m: 0 },
                memory: { total_bytes: 1000, used_bytes: 200, usage_percent: 20 },
                disks: [{ device: '/dev/sdb1', mount_point: '/data', filesystem: 'ext4', total_bytes: 1000, used_bytes: 500, usage_percent: 50 }],
                temperatures: [],
                top_processes: [{ pid: 1, name: 'TheWatcherAgent.exe', status: 'running', cpu_percent: 1, memory_rss_bytes: 1, num_threads: 1 }],
                networks: [{ interface_name: 'eth0', ip_address: '192.0.2.10', mac_address: '', bytes_sent_per_sec: 10_000_000, bytes_recv_per_sec: 10_000_000, errors_in: 0, errors_out: 0, drops_in: 0, drops_out: 0, is_up: true }],
                os_name: 'Linux',
                os_version: '6',
                hostname: 'host-1',
                platform: 'linux',
                uptime_seconds: 60,
            },
        };
        const rows = toDashboardAgents([
            agent({
                collector_config: {
                    ...DEFAULT_COLLECTOR_CONFIG,
                    cpu: { ...DEFAULT_PERCENT_THRESHOLDS, warning_percent: 50, degraded_percent: 70, critical_percent: 90 },
                    networks: [{ interface_name: 'eth0', enabled: true, thresholds: DEFAULT_NETWORK_THRESHOLDS }],
                    processes: [{ name: 'TheWatcherAgent.exe', expected_count: 2, enabled: true }],
                },
            }),
        ], [metrics]);
        expect(rows[0].components.find((component) => component.key === 'cpu')).toMatchObject({ color: 'yellow' });
        expect(rows[0].components.find((component) => component.key === 'network')).toMatchObject({ color: 'yellow' });
        expect(rows[0].components.find((component) => component.key === 'processes')).toMatchObject({ color: 'red' });
    });
});
describe('GIVEN overview agents assigned to groups', () => {
    const groups = [
        { group_id: 1, name: 'Production', built_in: false },
        { group_id: 2, name: 'Databases', built_in: false },
    ];
    it('WHEN overview groups are built THEN agents appear under every assigned group with ungrouped agents separated', () => {
        const rows = toDashboardAgents([
            agent({ agent_id: 'agent-prod', hostname: 'prod-1', group_ids: [1] }),
            agent({ agent_id: 'agent-db', hostname: 'db-1', group_ids: [1, 2] }),
            agent({ agent_id: 'agent-free', hostname: 'free-1', group_ids: [] }),
        ], []);
        const grouped = groupOverviewAgents(rows, groups, 'all');
        expect(grouped.map((group) => group.name)).toEqual(['Databases', 'Production', 'Ungrouped']);
        expect(grouped.find((group) => group.name === 'Databases')?.agents.map((row) => row.id)).toEqual(['agent-db']);
        expect(grouped.find((group) => group.name === 'Production')?.agents.map((row) => row.id)).toEqual([
            'agent-db',
            'agent-prod',
        ]);
        expect(grouped.find((group) => group.name === 'Ungrouped')?.agents.map((row) => row.id)).toEqual(['agent-free']);
    });
    it('WHEN a group filter is selected THEN only agents in that group are returned', () => {
        const rows = toDashboardAgents([
            agent({ agent_id: 'agent-prod', hostname: 'prod-1', group_ids: [1] }),
            agent({ agent_id: 'agent-db', hostname: 'db-1', group_ids: [2] }),
            agent({ agent_id: 'agent-free', hostname: 'free-1', group_ids: [] }),
        ], []);
        const grouped = groupOverviewAgents(rows, groups, '2');
        expect(grouped).toHaveLength(1);
        expect(grouped[0].name).toBe('Databases');
        expect(grouped[0].agents.map((row) => row.id)).toEqual(['agent-db']);
    });
});
describe('GIVEN alerts mixed acknowledged and unacknowledged', () => {
    it('WHEN dashboard agents are built THEN alertColor is red only when an unacknowledged alert exists', () => {
        const agents = [
            agent({ agent_id: 'agent-acked' }),
            agent({ agent_id: 'agent-unacked' }),
            agent({ agent_id: 'agent-clean' }),
        ];
        const alerts = [
            { alert_id: 1, agent_id: 'agent-acked', indicator: 'cpu', old_status: 'green', new_status: 'red', message: '', created_at: 1, acknowledged_by: 'admin', acknowledged_at: 999, deleted_at: 0, note: '', escalated_at: 0 },
            { alert_id: 2, agent_id: 'agent-unacked', indicator: 'cpu', old_status: 'green', new_status: 'red', message: '', created_at: 2, acknowledged_by: '', acknowledged_at: 0, deleted_at: 0, note: '', escalated_at: 0 },
        ];
        const rows = toDashboardAgents(agents, [], alerts);
        expect(rows.find((r) => r.id === 'agent-acked')?.alertColor).toBe('green');
        expect(rows.find((r) => r.id === 'agent-unacked')?.alertColor).toBe('red');
        expect(rows.find((r) => r.id === 'agent-clean')?.alertColor).toBe('green');
    });
});
describe('GIVEN alerts and known dashboard agents', () => {
    it('WHEN display alerts are built THEN the hostname is primary and the agent id remains available', () => {
        const rows = toDashboardAgents([agent({ agent_id: 'agent-prod', hostname: 'prod-1' })], []);
        const alerts = [
            {
                alert_id: 1,
                agent_id: 'agent-prod',
                indicator: 'cpu',
                old_status: 'green',
                new_status: 'yellow',
                message: 'cpu changed from green to yellow',
                created_at: 1000,
                acknowledged_by: '',
                acknowledged_at: 0,
                deleted_at: 0,
                note: '',
                escalated_at: 0,
            },
            {
                alert_id: 2,
                agent_id: 'agent-missing',
                indicator: 'memory',
                old_status: 'green',
                new_status: 'red',
                message: 'memory changed from green to red',
                created_at: 2000,
                acknowledged_by: '',
                acknowledged_at: 0,
                deleted_at: 0,
                note: '',
                escalated_at: 0,
            },
        ];
        const displayAlerts = toDisplayAlerts(alerts, rows);
        expect(displayAlerts[0]).toMatchObject({
            agentName: 'prod-1',
            agentId: 'agent-prod',
        });
        expect(displayAlerts[1]).toMatchObject({
            agentName: 'agent-missing',
            agentId: 'agent-missing',
        });
    });
});
describe('GIVEN formatBytes', () => {
    it('WHEN given zero or negative values THEN returns "0 B"', () => {
        expect(formatBytes(0)).toBe('0 B');
        expect(formatBytes(-1)).toBe('0 B');
        expect(formatBytes(Number.NaN)).toBe('0 B');
    });
    it('WHEN given byte-scale values THEN returns bytes without a decimal', () => {
        expect(formatBytes(1)).toBe('1 B');
        expect(formatBytes(512)).toBe('512 B');
        expect(formatBytes(1023)).toBe('1023 B');
    });
    it('WHEN given kilobyte-scale values THEN returns KB with one decimal for small values', () => {
        expect(formatBytes(1024)).toBe('1.0 KB');
        expect(formatBytes(1536)).toBe('1.5 KB');
        expect(formatBytes(10 * 1024)).toBe('10 KB');
    });
    it('WHEN given megabyte-scale values THEN returns MB', () => {
        expect(formatBytes(1024 * 1024)).toBe('1.0 MB');
        expect(formatBytes(500 * 1024 * 1024)).toBe('500 MB');
    });
    it('WHEN given gigabyte-scale values THEN returns GB with one decimal for small values', () => {
        expect(formatBytes(8 * 1024 * 1024 * 1024)).toBe('8.0 GB');
        expect(formatBytes(100 * 1024 * 1024 * 1024)).toBe('100 GB');
    });
});
describe('GIVEN formatDuration', () => {
    it('WHEN given zero or negative values THEN returns "unknown"', () => {
        expect(formatDuration(0)).toBe('unknown');
        expect(formatDuration(-60)).toBe('unknown');
        expect(formatDuration(Number.NaN)).toBe('unknown');
    });
    it('WHEN given sub-hour durations THEN returns minutes only', () => {
        expect(formatDuration(60)).toBe('1m');
        expect(formatDuration(3540)).toBe('59m');
    });
    it('WHEN given hour-scale durations THEN returns hours and minutes', () => {
        expect(formatDuration(3600)).toBe('1h 0m');
        expect(formatDuration(3600 + 30 * 60)).toBe('1h 30m');
        expect(formatDuration(23 * 3600 + 59 * 60)).toBe('23h 59m');
    });
    it('WHEN given day-scale durations THEN returns days, hours, and minutes', () => {
        expect(formatDuration(86400)).toBe('1d 0h 0m');
        expect(formatDuration(2 * 86400 + 6 * 3600)).toBe('2d 6h 0m');
        expect(formatDuration(2 * 86400 + 6 * 3600 + 25 * 60)).toBe('2d 6h 25m');
    });
});
describe('GIVEN isUptimeAlarm', () => {
    it('WHEN uptime is under one hour and positive THEN returns true', () => {
        expect(isUptimeAlarm(1)).toBe(true);
        expect(isUptimeAlarm(60)).toBe(true);
        expect(isUptimeAlarm(3599)).toBe(true);
    });
    it('WHEN uptime is one hour or more THEN returns false', () => {
        expect(isUptimeAlarm(3600)).toBe(false);
        expect(isUptimeAlarm(86400)).toBe(false);
    });
    it('WHEN uptime is zero or invalid THEN returns false', () => {
        expect(isUptimeAlarm(0)).toBe(false);
        expect(isUptimeAlarm(-1)).toBe(false);
        expect(isUptimeAlarm(Number.NaN)).toBe(false);
    });
});
describe('GIVEN classifyNetworkMbps', () => {
    it('WHEN traffic is below warning threshold THEN color is green', () => {
        expect(classifyNetworkMbps(0)).toBe('green');
        expect(classifyNetworkMbps(99)).toBe('green');
    });
    it('WHEN traffic crosses warning THEN color is yellow', () => {
        expect(classifyNetworkMbps(100)).toBe('yellow');
        expect(classifyNetworkMbps(149)).toBe('yellow');
    });
    it('WHEN traffic crosses degraded THEN color is amber', () => {
        expect(classifyNetworkMbps(200)).toBe('amber');
    });
    it('WHEN traffic crosses critical THEN color is red', () => {
        expect(classifyNetworkMbps(300)).toBe('red');
    });
    it('WHEN traffic uses custom thresholds THEN they override the defaults', () => {
        const custom = { warning_mbps: 10, degraded_mbps: 20, critical_mbps: 30 };
        expect(classifyNetworkMbps(5, custom)).toBe('green');
        expect(classifyNetworkMbps(15, custom)).toBe('yellow');
        expect(classifyNetworkMbps(25, custom)).toBe('amber');
        expect(classifyNetworkMbps(35, custom)).toBe('red');
    });
    it('WHEN given a non-finite value THEN color is grey', () => {
        expect(classifyNetworkMbps(Number.NaN)).toBe('grey');
        expect(classifyNetworkMbps(Infinity)).toBe('grey');
    });
});
describe('GIVEN worstColor', () => {
    it('WHEN all colors are green THEN returns green', () => {
        expect(worstColor(['green', 'green'])).toBe('green');
    });
    it('WHEN the array is empty THEN returns green', () => {
        expect(worstColor([])).toBe('green');
    });
    it('WHEN colors include red THEN red wins regardless of position', () => {
        expect(worstColor(['green', 'red', 'yellow'])).toBe('red');
        expect(worstColor(['red', 'amber'])).toBe('red');
    });
    it('WHEN severity order is green < grey < yellow < amber < red THEN highest wins', () => {
        expect(worstColor(['green', 'grey'])).toBe('grey');
        expect(worstColor(['grey', 'yellow'])).toBe('yellow');
        expect(worstColor(['yellow', 'amber'])).toBe('amber');
        expect(worstColor(['amber', 'red'])).toBe('red');
    });
});
describe('GIVEN hostStatus', () => {
    it('WHEN the agent is in maintenance THEN status is blue regardless of components', () => {
        const result = hostStatus({ maintenance: true, components: [{ key: 'cpu', label: 'CPU', color: 'red', value: '99%', detail: '' }] });
        expect(result).toBe('blue');
    });
    it('WHEN all components are grey (no data) THEN status is promoted to yellow', () => {
        const result = hostStatus({ maintenance: false, components: [{ key: 'cpu', label: 'CPU', color: 'grey', value: 'no data', detail: '' }] });
        expect(result).toBe('yellow');
    });
    it('WHEN components include a red component THEN status is red', () => {
        const result = hostStatus({
            maintenance: false,
            components: [
                { key: 'cpu', label: 'CPU', color: 'green', value: '10%', detail: '' },
                { key: 'memory', label: 'Memory', color: 'red', value: '98%', detail: '' },
            ],
        });
        expect(result).toBe('red');
    });
});
describe('GIVEN collectorConfigWithDefaults', () => {
    it('WHEN called with undefined THEN returns the default config', () => {
        const config = collectorConfigWithDefaults(undefined);
        expect(config.cpu).toEqual(DEFAULT_PERCENT_THRESHOLDS);
        expect(config.memory).toEqual(DEFAULT_PERCENT_THRESHOLDS);
        expect(config.cpu_readings).toBe(DEFAULT_COLLECTOR_CONFIG.cpu_readings);
    });
    it('WHEN called with a partial config THEN merges with defaults', () => {
        const partial = { cpu: { warning_percent: 50, degraded_percent: 70, critical_percent: 90 } };
        const config = collectorConfigWithDefaults(partial);
        expect(config.cpu.warning_percent).toBe(50);
        expect(config.memory).toEqual(DEFAULT_PERCENT_THRESHOLDS);
    });
});
describe('GIVEN toDashboardAgents with metric components', () => {
    function baseAgent(overrides = {}) {
        return {
            agent_id: 'comp-agent',
            hostname: 'comp-host',
            platform: 'linux',
            curve_public_key_z85: '',
            approved: true,
            rejected: false,
            connected: true,
            maintenance: false,
            maintenance_reason: '',
            maintenance_until: 0,
            collection_interval: 30,
            heartbeat_interval: 5,
            process_limit: 25,
            first_seen: 1,
            last_seen: 1,
            description: '',
            runbook_markdown: '',
            ...overrides,
        };
    }
    function baseSnapshot(agentId, overrides = {}) {
        return {
            agent_id: agentId,
            timestamp_ms: 1000,
            metrics: {
                cpu: { usage_percent: 10, num_logical_cores: 4, load_avg_1m: 0.5 },
                memory: { total_bytes: 8 * 1024 * 1024 * 1024, used_bytes: 2 * 1024 * 1024 * 1024, usage_percent: 25 },
                disks: [{ device: '/dev/sda1', mount_point: '/', filesystem: 'ext4', total_bytes: 100e9, used_bytes: 30e9, usage_percent: 30 }],
                temperatures: [{ sensor_name: 'cpu', sensor_label: 'core0', temperature_celsius: 45 }],
                top_processes: [{ pid: 1, name: 'init', status: 'running', cpu_percent: 1, memory_rss_bytes: 1000, num_threads: 1 }],
                networks: [{ interface_name: 'eth0', ip_address: '192.0.2.10', mac_address: '', bytes_sent_per_sec: 1e6, bytes_recv_per_sec: 1e6, errors_in: 0, errors_out: 0, drops_in: 0, drops_out: 0, is_up: true }],
                os_name: 'Linux',
                os_version: '6.1',
                hostname: 'comp-host',
                platform: 'linux',
                uptime_seconds: 3600,
                ...overrides,
            },
        };
    }
    it('WHEN disk usage is high THEN disk component color reflects severity', () => {
        const snapshot = baseSnapshot('comp-agent');
        snapshot.metrics.disks[0].usage_percent = 96;
        const [dashAgent] = toDashboardAgents([baseAgent()], [snapshot]);
        const disk = dashAgent.components.find((c) => c.key === 'disk');
        expect(disk.color).toBe('red');
        expect(disk.value).toBe('96%');
    });
    it('WHEN a disk monitor config is disabled THEN that disk is excluded from the health color', () => {
        const agentWithConfig = baseAgent({
            collector_config: {
                ...DEFAULT_COLLECTOR_CONFIG,
                disks: [{ mount_point: '/', device: '/dev/sda1', enabled: false, thresholds: DEFAULT_PERCENT_THRESHOLDS }],
            },
        });
        const snapshot = baseSnapshot('comp-agent');
        snapshot.metrics.disks[0].usage_percent = 99;
        const [dashAgent] = toDashboardAgents([agentWithConfig], [snapshot]);
        const disk = dashAgent.components.find((c) => c.key === 'disk');
        expect(disk.color).toBe('grey');
        expect(disk.value).toBe('none');
    });
    it('WHEN metrics include temperature sensors THEN dashboard components omit temperature status', () => {
        const snapshot = baseSnapshot('comp-agent');
        snapshot.metrics.temperatures[0].temperature_celsius = 96;
        const [dashAgent] = toDashboardAgents([baseAgent()], [snapshot]);
        expect(dashAgent.components.some((component) => component.key === 'temperature')).toBe(false);
    });
    it('WHEN a non-loopback network has an IP THEN dashboard rows expose it for display', () => {
        const snapshot = baseSnapshot('comp-agent');
        snapshot.metrics.networks[0].ip_address = '192.0.2.10';
        const [dashAgent] = toDashboardAgents([baseAgent()], [snapshot]);
        expect(primaryIpAddress(snapshot.metrics)).toBe('192.0.2.10');
        expect(dashAgent.ipAddress).toBe('192.0.2.10');
    });
    it('WHEN a network interface has a burst of errors in the latest interval THEN network component is red', () => {
        const snapshot = baseSnapshot('comp-agent');
        snapshot.metrics.networks[0].errors_in = NETWORK_ERROR_DELTA_THRESHOLD;
        const [dashAgent] = toDashboardAgents([baseAgent()], [snapshot]);
        const net = dashAgent.components.find((c) => c.key === 'network');
        expect(net.color).toBe('red');
        expect(net.detail).toMatch(/errors/);
    });
    it('WHEN an interface has only a few errors or drops in the latest interval THEN network component stays healthy', () => {
        const snapshot = baseSnapshot('comp-agent');
        snapshot.metrics.networks[0].errors_in = 1;
        snapshot.metrics.networks[0].drops_out = NETWORK_ERROR_DELTA_THRESHOLD - 2;
        const [dashAgent] = toDashboardAgents([baseAgent()], [snapshot]);
        const net = dashAgent.components.find((c) => c.key === 'network');
        expect(net.color).not.toBe('red');
    });
    it('WHEN an error blip is followed by a clean interval THEN network component recovers', () => {
        const blip = baseSnapshot('comp-agent');
        blip.metrics.networks[0].drops_in = 500;
        const clean = baseSnapshot('comp-agent');
        expect(toDashboardAgents([baseAgent()], [blip])[0].components.find((c) => c.key === 'network').color).toBe('red');
        expect(toDashboardAgents([baseAgent()], [clean])[0].components.find((c) => c.key === 'network').color).not.toBe('red');
    });
    it('WHEN an interface explicitly configured for monitoring is down THEN network component is red', () => {
        const snapshot = baseSnapshot('comp-agent');
        snapshot.metrics.networks[0].is_up = false;
        const agent = baseAgent({
            collector_config: {
                ...DEFAULT_COLLECTOR_CONFIG,
                networks: [{ interface_name: 'eth0', enabled: true, thresholds: DEFAULT_NETWORK_THRESHOLDS }],
            },
        });
        const [dashAgent] = toDashboardAgents([agent], [snapshot]);
        const net = dashAgent.components.find((c) => c.key === 'network');
        expect(net.color).toBe('red');
    });
    it('WHEN the only interfaces are loopback, pseudo or down and no config is set THEN none are monitored', () => {
        const snapshot = baseSnapshot('comp-agent');
        const template = snapshot.metrics.networks[0];
        snapshot.metrics.networks = [
            { ...template, interface_name: 'lo0', is_loopback: true },
            { ...template, interface_name: 'pflog0', is_up: false },
            { ...template, interface_name: 'enc0' },
            { ...template, interface_name: 'Loopback Pseudo-Interface 1' },
            { ...template, interface_name: 'Wi-Fi', is_up: false },
        ];
        const [dashAgent] = toDashboardAgents([baseAgent()], [snapshot]);
        const net = dashAgent.components.find((c) => c.key === 'network');
        expect(net.color).toBe('grey');
        expect(net.value).toBe('none');
    });
    it('WHEN the loopback interface is the only one and no config is set THEN it is excluded from monitoring', () => {
        const snapshot = baseSnapshot('comp-agent');
        snapshot.metrics.networks = [{ interface_name: 'lo', ip_address: '127.0.0.1', mac_address: '', bytes_sent_per_sec: 0, bytes_recv_per_sec: 0, errors_in: 0, errors_out: 0, drops_in: 0, drops_out: 0, is_up: true }];
        const [dashAgent] = toDashboardAgents([baseAgent()], [snapshot]);
        const net = dashAgent.components.find((c) => c.key === 'network');
        expect(net.color).toBe('grey');
        expect(net.value).toBe('none');
    });
    it('WHEN memory usage is high THEN memory component reflects severity', () => {
        const snapshot = baseSnapshot('comp-agent');
        snapshot.metrics.memory.usage_percent = 95;
        const [dashAgent] = toDashboardAgents([baseAgent()], [snapshot]);
        const mem = dashAgent.components.find((c) => c.key === 'memory');
        expect(mem.color).toBe('red');
        expect(mem.value).toBe('95%');
    });
    it('WHEN the agent description is set THEN toDashboardAgents maps it through', () => {
        const agentWithDesc = baseAgent({ description: 'Primary web server' });
        const [dashAgent] = toDashboardAgents([agentWithDesc], []);
        expect(dashAgent.description).toBe('Primary web server');
    });
});
