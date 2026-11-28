import React from 'react';
import { parseMarkdownBlocks, safeMarkdownHref } from '../uiLogic';

function renderInlineMarkdown(text: string): React.ReactNode[] {
  const nodes: React.ReactNode[] = [];
  const linkPattern = /\[([^\]]+)]\(([^)]+)\)/g;
  let lastIndex = 0;
  let match: RegExpExecArray | null;
  while ((match = linkPattern.exec(text)) !== null) {
    if (match.index > lastIndex) nodes.push(text.slice(lastIndex, match.index));
    const href = safeMarkdownHref(match[2]);
    nodes.push(
      href ? (
        <a href={href} key={`${match.index}:${href}`} rel="noopener noreferrer" target="_blank">
          {match[1]}
        </a>
      ) : (
        match[1]
      ),
    );
    lastIndex = match.index + match[0].length;
  }
  if (lastIndex < text.length) nodes.push(text.slice(lastIndex));
  return nodes;
}

export function RunbookMarkdown({ markdown }: { markdown: string }) {
  const blocks = parseMarkdownBlocks(markdown);
  if (blocks.length === 0) return <div className="banner">No runbook has been configured for this agent.</div>;
  return (
    <div className="runbook-markdown">
      {blocks.map((block, index) => {
        if (block.type === 'heading') {
          const Heading = `h${block.level}` as 'h1' | 'h2' | 'h3';
          return <Heading key={index}>{renderInlineMarkdown(block.text)}</Heading>;
        }
        if (block.type === 'list') {
          return (
            <ul key={index}>
              {block.items.map((item, itemIndex) => <li key={itemIndex}>{renderInlineMarkdown(item)}</li>)}
            </ul>
          );
        }
        if (block.type === 'code') return <pre key={index}><code>{block.text}</code></pre>;
        return <p key={index}>{renderInlineMarkdown(block.text)}</p>;
      })}
    </div>
  );
}

