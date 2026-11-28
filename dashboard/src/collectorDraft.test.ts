import { describe, expect, it } from 'vitest';
import { buildCollectorDraft } from './collectorDraft';
import type { DashboardAgent } from './models';

// Only the fields buildCollectorDraft reads; the rest of DashboardAgent is irrelevant here.
function agentWith(overrides: Record<string, unknown>): DashboardAgent {
  return {
    collectionInterval: 30,
    heartbeatInterval: 5,
    processLimit: 10,
    groupIds: [2],
    runbookMarkdown: '## CPU',
    collectorConfig: undefined,
    ...overrides,
  } as unknown as DashboardAgent;
}

describe('GIVEN an agent opened in the configure modal', () => {
  it('WHEN the draft is built THEN agent-level settings are copied and defaults fill the collector config', () => {
    const draft = buildCollectorDraft(agentWith({}));

    expect(draft.collection_interval).toBe(30);
    expect(draft.heartbeat_interval).toBe(5);
    expect(draft.process_limit).toBe(10);
    expect(draft.group_ids).toEqual([2]);
    expect(draft.runbook_markdown).toBe('## CPU');
    expect(draft.collector_config.cpu.warning_percent).toBe(80);
    expect(draft.collector_config.disks).toEqual([]);
  });

  it('WHEN the agent reports disks and interfaces that are not configured THEN they are offered enabled, except loopback', () => {
    const draft = buildCollectorDraft(
      agentWith({
        metrics: {
          disks: [{ mount_point: '/data', device: '/dev/sdb1' }],
          networks: [{ interface_name: 'eth0' }, { interface_name: 'lo' }],
        },
      }),
    );

    expect(draft.collector_config.disks.map((d) => [d.mount_point, d.enabled])).toEqual([['/data', true]]);
    expect(draft.collector_config.networks.map((n) => [n.interface_name, n.enabled])).toEqual([['eth0', true]]);
  });

  it('WHEN editing the draft THEN the agent snapshot is not mutated', () => {
    const agent = agentWith({ metrics: { disks: [{ mount_point: '/', device: 'sda' }], networks: [] } });
    const draft = buildCollectorDraft(agent);
    draft.collector_config.cpu.warning_percent = 1;

    expect(buildCollectorDraft(agent).collector_config.cpu.warning_percent).toBe(80);
  });
});
