import React from 'react';
import { parseInlineMarkdown, parseMarkdownBlocks, type InlineNode } from '../uiLogic';

/** Renders parsed inline nodes as React elements. Text is never injected as HTML. */
function renderInline(nodes: InlineNode[]): React.ReactNode[] {
  return nodes.map((node, index) => {
    switch (node.type) {
      case 'text':
        // A line break inside a paragraph is kept as <br>.
        return <React.Fragment key={index}>{node.text.split('\n').flatMap((line, lineIndex) => (lineIndex === 0 ? [line] : [<br key={lineIndex} />, line]))}</React.Fragment>;
      case 'code':
        return <code key={index}>{node.text}</code>;
      case 'bold':
        return <strong key={index}>{renderInline(node.children)}</strong>;
      case 'italic':
        return <em key={index}>{renderInline(node.children)}</em>;
      case 'link':
        return node.href ? (
          <a href={node.href} key={index} rel="noopener noreferrer" target="_blank">{renderInline(node.children)}</a>
        ) : (
          <React.Fragment key={index}>{renderInline(node.children)}</React.Fragment> // an unsafe target shows its text only
        );
    }
  });
}

const inline = (text: string) => renderInline(parseInlineMarkdown(text));

export function RunbookMarkdown({ markdown }: { markdown: string }) {
  const blocks = parseMarkdownBlocks(markdown);
  if (blocks.length === 0) return <div className="banner">No runbook has been configured for this agent.</div>;
  return (
    <div className="runbook-markdown">
      {blocks.map((block, index) => {
        if (block.type === 'heading') {
          const Heading = `h${block.level}` as 'h1' | 'h2' | 'h3';
          return <Heading key={index}>{inline(block.text)}</Heading>;
        }
        if (block.type === 'list') {
          const List = block.ordered ? 'ol' : 'ul';
          return (
            <List key={index}>
              {block.items.map((item, itemIndex) => <li key={itemIndex}>{inline(item)}</li>)}
            </List>
          );
        }
        if (block.type === 'code') return <pre key={index}><code>{block.text}</code></pre>;
        return <p key={index}>{inline(block.text)}</p>;
      })}
    </div>
  );
}
