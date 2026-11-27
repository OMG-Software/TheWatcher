import React from 'react';
import { ActionFailureContext } from '../hooks/useRunAction';

/** Shows the failure of one specific action, next to the form or modal that started it. */
export function ActionError({ actionKey }: { actionKey: string }) {
  const failure = React.useContext(ActionFailureContext);
  if (failure?.key !== actionKey) return null;
  return (
    <div className="banner error" role="alert">
      {failure.message}
    </div>
  );
}

/** Page-level banner for failures that have no form of their own. Dismissible. */
export function ActionErrorBanner({ onDismiss }: { onDismiss: () => void }) {
  const failure = React.useContext(ActionFailureContext);
  if (!failure || failure.inline) return null;
  return (
    <div className="banner error" role="alert">
      Action failed: {failure.message}
      <button className="text-button banner-action" onClick={onDismiss} type="button">Dismiss</button>
    </div>
  );
}
