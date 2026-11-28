import React from 'react';
import type { ViewRecord } from '../models';
import { parseViewHash } from '../uiLogic';

/**
 * Tracks the custom view filtering the monitoring page, mirrored into the URL hash so it is bookmarkable.
 *
 * Once the views have loaded, a view id that is not among them (deleted, or never visible to this
 * user, as with an old bookmark) is dropped, together with its hash. Until then, `view` is null and
 * `resolved` is false so the page can show a loading state, not an empty fleet.
 * `viewsKnown` must be false while the views are loading or failed to load: a failure must not
 * drop a valid view.
 */
export function useActiveView(views: ViewRecord[], viewsKnown: boolean) {
  const [activeViewId, setActiveViewId] = React.useState<number | null>(() => parseViewHash(window.location.hash));

  const view = activeViewId == null ? null : (views.find((candidate) => candidate.view_id === activeViewId) ?? null);
  // Adjusting state during render is React's pattern for state derived from props.
  if (activeViewId != null && viewsKnown && view === null) setActiveViewId(null);

  React.useEffect(() => {
    if (activeViewId != null) {
      window.location.hash = `view-${activeViewId}`;
    } else if (window.location.hash.startsWith('#view-')) {
      window.history.replaceState(null, '', window.location.pathname + window.location.search);
    }
  }, [activeViewId]);

  return { activeViewId, setActiveViewId, view, resolved: activeViewId == null || view !== null };
}
