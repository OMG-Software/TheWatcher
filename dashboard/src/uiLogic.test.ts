import { describe, expect, it } from 'vitest';
import { NAV_ITEMS, accountTypeLabel, colorLabel, isInsecureOrigin, parseMarkdownBlocks, parseViewHash, safeMarkdownHref } from './uiLogic';

describe('GIVEN dashboard navigation rules', () => {
  it('WHEN the nav item list is read THEN it matches the requested top-level order without pending enrollments', () => {
    expect(NAV_ITEMS.map((item) => item.label)).toEqual([
      'Monitoring',
      'Views',
      'Alerts',
      'Silences',
      'Maintenance',
      'Agents',
      'Users',
      'Settings',
    ]);
    expect(NAV_ITEMS.map((item) => item.view)).not.toContain('pending');
  });
});

describe('GIVEN account role values', () => {
  it('WHEN account type labels are displayed THEN they are separate from group names', () => {
    expect(accountTypeLabel('global_admin')).toBe('Global admin');
    expect(accountTypeLabel('global_operator')).toBe('Global operator');
    expect(accountTypeLabel('global_viewer')).toBe('Global viewer');
    expect(accountTypeLabel('group_admin')).toBe('Group admin');
    expect(accountTypeLabel('group_operator')).toBe('Group operator');
    expect(accountTypeLabel('group_viewer')).toBe('Group viewer');
  });
});

describe('GIVEN agent runbook markdown', () => {
  it('WHEN markdown is parsed for the alert modal THEN rendered block types are produced', () => {
    const blocks = parseMarkdownBlocks([
      '# CPU critical',
      '',
      'Check service health before restarting.',
      '- Open dashboard',
      '- Restart worker',
      '```',
      'systemctl status watcher',
      '```',
    ].join('\n'));

    expect(blocks).toEqual([
      { type: 'heading', level: 1, text: 'CPU critical' },
      { type: 'paragraph', text: 'Check service health before restarting.' },
      { type: 'list', items: ['Open dashboard', 'Restart worker'] },
      { type: 'code', text: 'systemctl status watcher' },
    ]);
  });

  it('WHEN markdown links are rendered THEN only http, https, and mailto targets are allowed', () => {
    expect(safeMarkdownHref('https://wiki.example.com/runbook')).toBe('https://wiki.example.com/runbook');
    expect(safeMarkdownHref('mailto:oncall@example.com')).toBe('mailto:oncall@example.com');
    expect(safeMarkdownHref('javascript:alert(1)')).toBeNull();
  });
});

describe('GIVEN health colours shown in the dashboard', () => {
  it('WHEN each colour is labelled THEN it maps to its operator-facing status word', () => {
    expect(colorLabel('green')).toBe('Healthy');
    expect(colorLabel('yellow')).toBe('Warning');
    expect(colorLabel('amber')).toBe('Degraded');
    expect(colorLabel('red')).toBe('Critical');
    expect(colorLabel('grey')).toBe('No Data');
    expect(colorLabel('blue')).toBe('Maintenance');
  });
});

describe('GIVEN a URL hash that may select a custom view', () => {
  it('WHEN the hash is #view-<id> THEN the numeric id is returned', () => {
    expect(parseViewHash('#view-42')).toBe(42);
  });

  it('WHEN the hash is empty, unrelated or malformed THEN no view is selected', () => {
    expect(parseViewHash('')).toBeNull();
    expect(parseViewHash('#alerts')).toBeNull();
    expect(parseViewHash('#view-')).toBeNull();
    expect(parseViewHash('#view-abc')).toBeNull();
    expect(parseViewHash('#view-3-extra')).toBeNull();
  });
});

describe('GIVEN the page origin', () => {
  it.each([
    ['https:', 'dash.example.com', false],
    ['http:', 'localhost', false],
    ['http:', '127.0.0.1', false],
    ['http:', '[::1]', false],
    ['http:', 'app.localhost', false],
    ['http:', '192.168.1.20', true],
    ['http:', 'dash.example.com', true],
  ])('WHEN the page is %s//%s THEN the insecure-origin warning is %s', (protocol, hostname, expected) => {
    expect(isInsecureOrigin({ protocol, hostname })).toBe(expected);
  });
});
