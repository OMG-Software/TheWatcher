import React from 'react';

/** The label stays while busy (a screen reader keeps the button's name); "…" and `aria-busy` show the wait. */
export function ActionButton({ busy, danger = false, disabled = false, icon, label, onClick, type = 'button' }: { busy: boolean; danger?: boolean; disabled?: boolean; icon: React.ReactNode; label: string; onClick: () => void; type?: 'button' | 'submit' }) {
  return (
    <button aria-busy={busy || undefined} className={`text-button ${danger ? 'danger' : ''}`} disabled={disabled || busy} onClick={onClick} type={type}>
      {icon}
      {label}{busy ? '…' : ''}
    </button>
  );
}
