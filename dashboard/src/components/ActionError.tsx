import React from 'react';
import { ActionFailureContext } from '../hooks/useRunAction';

/**
 * Shows the failure of one specific action, next to the form or modal that started it.
 * While it is on screen it tells the page banner it has taken that failure; when the form
 * closes, the banner shows the failure instead, so it is never lost.
 */
export function ActionError({ actionKey }: { actionKey: string }) {
  const { failure, register } = React.useContext(ActionFailureContext);
  React.useEffect(() => register(actionKey), [actionKey, register]);
  if (failure?.key !== actionKey) return null;
  return (
    <div className="banner error" role="alert">
      {failure.message}
    </div>
  );
}

/**
 * Page-level banner for failures that have no form of their own, including inline failures whose
 * form is gone. Dismissible. A failed change reads "Action failed: ..."; a saved change whose
 * reload failed already says so in its message and is not labelled as failed.
 */
export function ActionErrorBanner({ onDismiss }: { onDismiss: () => void }) {
  const { failure, hasRecipient } = React.useContext(ActionFailureContext);
  if (!failure || (failure.inline && hasRecipient(failure.key))) return null;
  return (
    <div className="banner error" role="alert">
      {failure.kind === 'action' ? `Action failed: ${failure.message}` : failure.message}
      <button className="text-button banner-action" onClick={onDismiss} type="button">Dismiss</button>
    </div>
  );
}
