import React from 'react';

export function AgentIdentity({ id, name, prefix, secondary }: { id: string; name: string; prefix?: React.ReactNode; secondary?: string }) {
  return (
    <div>
      <div className="server-name">
        {prefix}
        <span>{name || id}</span>
      </div>
      <div className="agent-id">{secondary ?? id}</div>
    </div>
  );
}

