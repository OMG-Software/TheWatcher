import type { HealthColor, UserRole } from './models';

export type DashboardView =
  | 'monitoring'
  | 'agents'
  | 'alerts'
  | 'users'
  | 'maintenance'
  | 'silences'
  | 'settings'
  | 'views';

export interface NavItem {
  view: DashboardView;
  label: string;
  requiresOperator?: boolean;
  requiresGlobalOperator?: boolean;
}

export const NAV_ITEMS: NavItem[] = [
  { view: 'monitoring', label: 'Monitoring' },
  { view: 'views', label: 'Views' },
  { view: 'alerts', label: 'Alerts' },
  { view: 'silences', label: 'Silences', requiresOperator: true },
  { view: 'maintenance', label: 'Maintenance', requiresOperator: true },
  { view: 'agents', label: 'Agents' },
  { view: 'users', label: 'Users' },
  { view: 'settings', label: 'Settings', requiresGlobalOperator: true },
];

const ACCOUNT_TYPE_LABELS: Record<UserRole, string> = {
  global_admin: 'Global admin',
  global_operator: 'Global operator',
  global_viewer: 'Global viewer',
  group_admin: 'Group admin',
  group_operator: 'Group operator',
  group_viewer: 'Group viewer',
};

export function accountTypeLabel(role: UserRole): string {
  return ACCOUNT_TYPE_LABELS[role];
}

export type MarkdownBlock =
  | { type: 'heading'; level: 1 | 2 | 3; text: string }
  | { type: 'paragraph'; text: string }
  | { type: 'list'; ordered: boolean; items: string[] }
  | { type: 'code'; text: string };

const isSpace = (char: string | undefined) => char === ' ' || char === '	';

/** "# text" to "### text". Scanned by hand: a regex with `\s+(.+)` backtracks badly on long whitespace runs. */
function parseHeading(line: string): { level: 1 | 2 | 3; text: string } | null {
  let level = 0;
  while (line[level] === '#') level += 1;
  if (level < 1 || level > 3 || !isSpace(line[level])) return null;
  const text = line.slice(level).trimStart();
  return text ? { level: level as 1 | 2 | 3, text } : null;
}

/** "- item", "* item", "1. item" or "1) item", scanned in one pass. */
function parseListItem(line: string): { ordered: boolean; text: string } | null {
  let marker = 0;
  let ordered = false;
  if (line[0] === '-' || line[0] === '*') {
    marker = 1;
  } else {
    while (line[marker] >= '0' && line[marker] <= '9') marker += 1;
    if (marker === 0 || (line[marker] !== '.' && line[marker] !== ')')) return null;
    marker += 1;
    ordered = true;
  }
  if (!isSpace(line[marker])) return null;
  const text = line.slice(marker).trimStart();
  return text ? { ordered, text } : null;
}

export function parseMarkdownBlocks(markdown: string): MarkdownBlock[] {
  const blocks: MarkdownBlock[] = [];
  const lines = markdown.replace(/\r\n/g, '\n').split('\n');
  let paragraph: string[] = [];
  let list: string[] = [];
  let listOrdered = false;
  let code: string[] | null = null;

  const flushParagraph = () => {
    if (paragraph.length > 0) {
      blocks.push({ type: 'paragraph', text: paragraph.join('\n') }); // line breaks are kept; the renderer shows them
      paragraph = [];
    }
  };
  const flushList = () => {
    if (list.length > 0) {
      blocks.push({ type: 'list', ordered: listOrdered, items: list });
      list = [];
    }
  };

  for (const line of lines) {
    if (line.trim().startsWith('```')) {
      if (code) {
        blocks.push({ type: 'code', text: code.join('\n') });
        code = null;
      } else {
        flushParagraph();
        flushList();
        code = [];
      }
      continue;
    }
    if (code) {
      code.push(line);
      continue;
    }

    const trimmed = line.trim();
    if (!trimmed) {
      flushParagraph();
      flushList();
      continue;
    }

    const heading = parseHeading(trimmed);
    if (heading) {
      flushParagraph();
      flushList();
      blocks.push({ type: 'heading', level: heading.level, text: heading.text });
      continue;
    }

    // A change between bullets and numbers starts a new list.
    const item = parseListItem(trimmed);
    if (item) {
      flushParagraph();
      if (list.length > 0 && listOrdered !== item.ordered) flushList();
      listOrdered = item.ordered;
      list.push(item.text);
      continue;
    }

    flushList();
    paragraph.push(trimmed);
  }

  if (code) blocks.push({ type: 'code', text: code.join('\n') });
  flushParagraph();
  flushList();
  return blocks;
}

export function safeMarkdownHref(href: string): string | null {
  const trimmed = href.trim();
  if (/^(https?:|mailto:)/i.test(trimmed)) return trimmed;
  return null;
}

const COLOR_LABELS: Record<HealthColor, string> = {
  green: 'Healthy',
  yellow: 'Warning',
  amber: 'Degraded',
  red: 'Critical',
  grey: 'No Data',
  blue: 'Maintenance',
};

export function colorLabel(color: HealthColor): string {
  return COLOR_LABELS[color];
}

/** Reads the custom-view id from a `#view-<id>` URL hash, or null when absent or malformed. */
export function parseViewHash(hash: string): number | null {
  const match = hash.match(/^#view-(\d+)$/);
  return match ? parseInt(match[1], 10) : null;
}

/**
 * True when the page is served over plain HTTP from a non-local host. The
 * session cookie is `Secure`, so the browser drops it there and every call
 * after login would be rejected as unauthenticated.
 */
export function isInsecureOrigin(location: { protocol: string; hostname: string }): boolean {
  if (location.protocol !== 'http:') return false;
  const host = location.hostname.toLowerCase();
  return !(host === 'localhost' || host.endsWith('.localhost') || host === '127.0.0.1' || host === '[::1]' || host === '::1');
}

export type InlineNode =
  | { type: 'text'; text: string }
  | { type: 'code'; text: string }
  | { type: 'bold'; children: InlineNode[] }
  | { type: 'italic'; children: InlineNode[] }
  | { type: 'link'; href: string | null; children: InlineNode[] };

/** Index of the `)` closing the `(` at `open`, counting nested pairs so URLs with parentheses survive; -1 if none. */
function matchingParen(text: string, open: number): number {
  let depth = 0;
  for (let i = open; i < text.length; i += 1) {
    if (text[i] === '(') depth += 1;
    else if (text[i] === ')' && --depth === 0) return i;
  }
  return -1;
}

/**
 * Inline markdown: `code`, **bold**, *italic* and [text](url). Everything else is plain text, and
 * nothing is ever interpreted as HTML: the result is data for React to render as text, and a
 * link's `href` is null unless it passes `safeMarkdownHref`.
 */
export function parseInlineMarkdown(text: string): InlineNode[] {
  const nodes: InlineNode[] = [];
  let plain = '';
  const flush = () => {
    if (plain) nodes.push({ type: 'text', text: plain });
    plain = '';
  };
  let i = 0;
  while (i < text.length) {
    const rest = text.slice(i);
    let match: RegExpExecArray | null;
    if ((match = /^`([^`]+)`/.exec(rest))) {
      flush();
      nodes.push({ type: 'code', text: match[1] });
      i += match[0].length;
    } else if ((match = /^\*\*(?=\S)(.+?)(?<=\S)\*\*/.exec(rest))) {
      flush();
      nodes.push({ type: 'bold', children: parseInlineMarkdown(match[1]) });
      i += match[0].length;
    } else if ((match = /^\*(?=[^\s*])(.+?)(?<=[^\s*])\*/.exec(rest))) {
      flush();
      nodes.push({ type: 'italic', children: parseInlineMarkdown(match[1]) });
      i += match[0].length;
    } else if ((match = /^\[([^\]]+)\]\(/.exec(rest))) {
      const close = matchingParen(rest, match[0].length - 1);
      if (close === -1) {
        plain += text[i];
        i += 1;
      } else {
        flush();
        nodes.push({ type: 'link', href: safeMarkdownHref(rest.slice(match[0].length, close)), children: parseInlineMarkdown(match[1]) });
        i += close + 1;
      }
    } else {
      plain += text[i];
      i += 1;
    }
  }
  flush();
  return nodes;
}
