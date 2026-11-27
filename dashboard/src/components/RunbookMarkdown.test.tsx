import { render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { RunbookMarkdown } from './RunbookMarkdown';

describe('GIVEN a runbook rendered for display', () => {
  it('WHEN it has a numbered list THEN it renders as an ordered list', () => {
    render(<RunbookMarkdown markdown={'1. Stop service\n2. Clear cache'} />);
    const list = screen.getByRole('list');
    expect(list.tagName).toBe('OL');
    expect(screen.getAllByRole('listitem').map((item) => item.textContent)).toEqual(['Stop service', 'Clear cache']);
  });

  it('WHEN it has inline formatting THEN code, bold and italic render as elements', () => {
    const { container } = render(<RunbookMarkdown markdown={'run `ls` **now** *later*'} />);
    expect(container.querySelector('code')?.textContent).toBe('ls');
    expect(container.querySelector('strong')?.textContent).toBe('now');
    expect(container.querySelector('em')?.textContent).toBe('later');
  });

  it('WHEN it contains raw HTML or a script link THEN nothing is injected', () => {
    const { container } = render(<RunbookMarkdown markdown={'<img src=x onerror=alert(1)> [bad](javascript:alert(1))'} />);
    expect(container.querySelector('img')).toBeNull();
    expect(container.querySelector('a')).toBeNull();
    expect(container.textContent).toContain('<img src=x onerror=alert(1)>');
  });

  it('WHEN a link is safe THEN it opens in a new tab without a referrer', () => {
    render(<RunbookMarkdown markdown="[wiki](https://example.com/a_(b))" />);
    const link = screen.getByRole('link', { name: 'wiki' });
    expect(link).toHaveAttribute('href', 'https://example.com/a_(b)');
    expect(link).toHaveAttribute('rel', 'noopener noreferrer');
  });
});
