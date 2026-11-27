import React from 'react';
import { parseViewHash } from '../uiLogic';

/** Tracks the custom view filtering the monitoring page, mirrored into the URL hash so it is bookmarkable. */
export function useActiveView() {
  const [activeViewId, setActiveViewId] = React.useState<number | null>(() => parseViewHash(window.location.hash));

  React.useEffect(() => {
    if (activeViewId != null) {
      window.location.hash = `view-${activeViewId}`;
    } else if (window.location.hash.startsWith('#view-')) {
      window.history.replaceState(null, '', window.location.pathname + window.location.search);
    }
  }, [activeViewId]);

  // Following a #view-N link while the dashboard is already open changes only the hash.
  React.useEffect(() => {
    const onHashChange = () => setActiveViewId(parseViewHash(window.location.hash));
    window.addEventListener('hashchange', onHashChange);
    return () => window.removeEventListener('hashchange', onHashChange);
  }, []);

  return { activeViewId, setActiveViewId };
}
