import { fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { deferred } from '../testing/fakeApi';
import { ActionFailureContext, useRunAction } from '../hooks/useRunAction';
import { ActionError, ActionErrorBanner } from './ActionError';

/** A page with the banner, an optional form that owns the inline error for "user:create", and a button that runs the action. */
function Harness({ action, formOpen, inline = true, refresh = async () => null }: { action: () => Promise<void>; formOpen: boolean; inline?: boolean; refresh?: () => Promise<string | null> }) {
  const run = useRunAction(refresh);
  return (
    <ActionFailureContext.Provider value={run.context}>
      <ActionErrorBanner onDismiss={() => run.clearFailure()} />
      {formOpen ? <ActionError actionKey="user:create" /> : null}
      <button onClick={() => void run.runAction('user:create', action, { inline })} type="button">Go</button>
    </ActionFailureContext.Provider>
  );
}

const go = () => fireEvent.click(screen.getByRole('button', { name: 'Go' }));

describe('GIVEN a failure that belongs to a form', () => {
  it('WHEN the form is still open THEN the error shows in the form and not in the page banner', async () => {
    render(<Harness action={() => Promise.reject(new Error('name taken'))} formOpen />);

    go();

    expect(await screen.findByText('name taken')).toBeInTheDocument();
    expect(screen.getAllByRole('alert')).toHaveLength(1);
    expect(screen.queryByText(/Action failed/)).toBeNull();
  });

  it('WHEN the form was closed before the failure arrived THEN the page banner shows it', async () => {
    const reply = deferred<void>();
    const { rerender } = render(<Harness action={() => reply.promise} formOpen />);
    go();

    rerender(<Harness action={() => reply.promise} formOpen={false} />); // the user navigated away
    reply.reject(new Error('name taken'));

    expect(await screen.findByText('Action failed: name taken')).toBeInTheDocument();
  });

  it('WHEN the form closes after its failure was shown THEN the failure moves to the page banner instead of vanishing', async () => {
    const action = () => Promise.reject(new Error('name taken'));
    const { rerender } = render(<Harness action={action} formOpen />);
    go();
    await screen.findByText('name taken');

    rerender(<Harness action={action} formOpen={false} />);

    expect(await screen.findByText('Action failed: name taken')).toBeInTheDocument();
  });

  it('WHEN the failure is not inline THEN the page banner shows it as a failed action', async () => {
    render(<Harness action={() => Promise.reject(new Error('boom'))} formOpen={false} inline={false} />);

    go();

    expect(await screen.findByText('Action failed: boom')).toBeInTheDocument();
  });
});

describe('GIVEN a change that was saved but whose refresh failed', () => {
  it('WHEN the banner shows it THEN it is not labelled as a failed action', async () => {
    const refresh = vi.fn().mockResolvedValue('server down');
    render(<Harness action={() => Promise.resolve()} formOpen={false} refresh={refresh} />);

    go();

    const banner = await screen.findByText(/Saved, but the dashboard could not be refreshed: server down/);
    expect(banner).toBeInTheDocument();
    expect(screen.queryByText(/Action failed/)).toBeNull();
  });
});
