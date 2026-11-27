import type { DashboardView } from '../uiLogic';

export function NavTab({ view, target, setView, label }: { view: DashboardView; target: DashboardView; setView: (view: DashboardView) => void; label: string }) {
  return (
    <button className={view === target ? 'active' : ''} onClick={() => setView(target)}>
      {label}
    </button>
  );
}

