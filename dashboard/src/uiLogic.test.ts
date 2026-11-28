import { describe, expect, it } from 'vitest';
import { NAV_ITEMS, accountTypeLabel, colorLabel, isInsecureOrigin, parseInlineMarkdown, parseMarkdownBlocks, parseViewHash, safeMarkdownHref } from './uiLogic';

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
      { type: 'list', ordered: false, items: ['Open dashboard', 'Restart worker'] },
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

describe('GIVEN runbook markdown with numbered lists and inline formatting (issue #113)', () => {
  it('WHEN a numbered list is parsed THEN it is an ordered list, separate from a bullet list', () => {
    const blocks = parseMarkdownBlocks(['1. Stop service', '2) Clear cache', '- note'].join('\n'));
    expect(blocks).toEqual([
      { type: 'list', ordered: true, items: ['Stop service', 'Clear cache'] },
      { type: 'list', ordered: false, items: ['note'] },
    ]);
  });

  it('WHEN a paragraph spans lines THEN the line breaks are kept', () => {
    expect(parseMarkdownBlocks('first\nsecond')).toEqual([{ type: 'paragraph', text: 'first\nsecond' }]);
  });

  it('WHEN text has code, bold and italic THEN they become inline nodes', () => {
    expect(parseInlineMarkdown('run `ls -l` **now**, *then* stop')).toEqual([
      { type: 'text', text: 'run ' },
      { type: 'code', text: 'ls -l' },
      { type: 'text', text: ' ' },
      { type: 'bold', children: [{ type: 'text', text: 'now' }] },
      { type: 'text', text: ', ' },
      { type: 'italic', children: [{ type: 'text', text: 'then' }] },
      { type: 'text', text: ' stop' },
    ]);
  });

  it('WHEN a link URL contains parentheses THEN the whole URL is kept', () => {
    expect(parseInlineMarkdown('[wiki](https://en.wikipedia.org/wiki/Foo_(bar))')).toEqual([
      { type: 'link', href: 'https://en.wikipedia.org/wiki/Foo_(bar)', children: [{ type: 'text', text: 'wiki' }] },
    ]);
  });

  it('WHEN a link target is not http, https or mailto THEN it has no href', () => {
    expect(parseInlineMarkdown('[x](javascript:alert(1))')).toEqual([{ type: 'link', href: null, children: [{ type: 'text', text: 'x' }] }]);
  });

  it('WHEN text has stray markers or HTML THEN it stays plain text', () => {
    expect(parseInlineMarkdown('2 * 3 and <b>x</b> [open')).toEqual([{ type: 'text', text: '2 * 3 and <b>x</b> [open' }]);
  });
});

describe('GIVEN hostile runbook lines (Sonar S8786)', () => {
  it('WHEN a line is a marker followed by very long whitespace THEN parsing stays fast and linear', () => {
    const started = performance.now();
    const blocks = parseMarkdownBlocks([`1.${' '.repeat(200_000)}x`, `-${' '.repeat(200_000)}`, `#${'\t'.repeat(200_000)}y`, `1${' '.repeat(200_000)}`].join('\n'));
    expect(performance.now() - started).toBeLessThan(500);
    expect(blocks.length).toBeGreaterThan(0);
  });

  it('WHEN markers are not followed by text THEN they stay a paragraph, and #### is not a heading', () => {
    expect(parseMarkdownBlocks('- ')).toEqual([{ type: 'paragraph', text: '-' }]);
    expect(parseMarkdownBlocks('#### big')).toEqual([{ type: 'paragraph', text: '#### big' }]);
    expect(parseMarkdownBlocks('## Title')).toEqual([{ type: 'heading', level: 2, text: 'Title' }]);
  });
});
