import React from 'react';

export function ActionButton({ busy, danger = false, disabled = false, icon, label, onClick, type = 'button' }: { busy: boolean; danger?: boolean; disabled?: boolean; icon: React.ReactNode; label: string; onClick: () => void; type?: 'button' | 'submit' }) {
  return (
    <button className={`text-button ${danger ? 'danger' : ''}`} disabled={disabled || busy} onClick={onClick} type={type}>
      {icon}
      {busy ? '...' : label}
    </button>
  );
}

