import type { AgentRecord, AlertRecord, AgentThresholds, AnomalyConfig, CollectorConfig, DashboardAgent, GroupRecord, HealthColor, MetricsSnapshot, NetworkThresholds, PercentThresholds, SystemMetrics } from './models';
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
export declare const DEFAULT_THRESHOLDS: AgentThresholds;
export declare const DEFAULT_ANOMALY_CONFIG: AnomalyConfig;
export declare const DEFAULT_PERCENT_THRESHOLDS: PercentThresholds;
export declare const DEFAULT_NETWORK_THRESHOLDS: NetworkThresholds;
export declare const DEFAULT_COLLECTOR_CONFIG: CollectorConfig;
export declare function collectorConfigWithDefaults(config?: CollectorConfig): CollectorConfig;
export declare function classifyPercent(value: number, thresholds?: PercentThresholds): HealthColor;
export declare function classifyNetworkMbps(value: number, thresholds?: NetworkThresholds): HealthColor;
export declare function worstColor(colors: HealthColor[]): HealthColor;
export declare function hostStatus(agent: Pick<DashboardAgent, 'maintenance' | 'components'>): HealthColor;
export declare function formatBytes(bytes: number): string;
export declare function formatDuration(seconds: number): string;
export declare const UPTIME_ALARM_SECONDS = 3600;
export declare function isUptimeAlarm(uptimeSeconds: number): boolean;
export declare const NETWORK_ERROR_DELTA_THRESHOLD = 10;
export declare function primaryIpAddress(metrics?: SystemMetrics): string;
export declare function toDashboardAgents(agents: AgentRecord[], snapshots: MetricsSnapshot[], alerts?: AlertRecord[]): DashboardAgent[];
export declare function agentStatus(agent: DashboardAgent): HealthColor;
export declare function groupOverviewAgents(agents: DashboardAgent[], groups: GroupRecord[], filter: OverviewGroupFilter): OverviewAgentGroup[];
export declare function summaryCounts(agents: DashboardAgent[]): Record<SummaryKey, number>;
export declare function toDisplayAlerts(alerts: AlertRecord[], agents: DashboardAgent[]): DisplayAlert[];
