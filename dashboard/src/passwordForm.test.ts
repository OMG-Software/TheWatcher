import { describe, expect, it } from 'vitest';
import {
  canChangePassword,
  closedPasswordForm,
  failSubmit,
  needsCurrentPassword,
  openPasswordForm,
  settlePasswordChange,
  submitPasswordChange,
} from './passwordForm';

describe('GIVEN who may change which password', () => {
  it('WHEN a global admin looks at any row THEN the Password button is offered', () => {
    expect(canChangePassword('global_admin', 1, 1)).toBe(true);
    expect(canChangePassword('global_admin', 1, 2)).toBe(true);
  });

  it('WHEN any other role looks at another user THEN it is hidden because the API refuses with 403', () => {
    for (const role of ['global_operator', 'global_viewer', 'group_admin', 'group_operator', 'group_viewer'] as const) {
      expect(canChangePassword(role, 1, 2)).toBe(false);
    }
  });

  it('WHEN any role looks at their own row THEN the button is offered', () => {
    for (const role of ['global_admin', 'global_operator', 'global_viewer', 'group_admin', 'group_operator', 'group_viewer'] as const) {
      expect(canChangePassword(role, 7, 7)).toBe(true);
    }
  });

  it('WHEN the own account is not known yet THEN only a global admin sees the button', () => {
    expect(canChangePassword('group_admin', undefined, 3)).toBe(false);
    expect(canChangePassword('global_admin', undefined, 3)).toBe(true);
  });

  it('WHEN the form targets the own account THEN the current password is required, otherwise not', () => {
    expect(needsCurrentPassword(openPasswordForm(7), 7)).toBe(true);
    expect(needsCurrentPassword(openPasswordForm(8), 7)).toBe(false);
    expect(needsCurrentPassword(closedPasswordForm, 7)).toBe(false);
  });
});

describe('GIVEN the change-password form', () => {
  const filled = { ...openPasswordForm(7), current: 'old-secret', value: 'new-secret' };

  it('WHEN the change succeeds THEN the form closes and both fields are cleared', async () => {
    const outcome = await submitPasswordChange(filled, () => Promise.resolve());
    expect(outcome).toEqual(closedPasswordForm);
    expect(outcome.current).toBe('');
    expect(outcome.value).toBe('');
  });

  it('WHEN the server refuses THEN the form stays open with the reason and keeps the new password', async () => {
    const outcome = await submitPasswordChange(filled, () => Promise.reject(new Error('403 Forbidden: current password is incorrect')));
    expect(outcome.userId).toBe(7);
    expect(outcome.error).toBe('403 Forbidden: current password is incorrect');
    expect(outcome.value).toBe('new-secret');
    expect(outcome.current).toBe('');
    expect(outcome.saving).toBe(false);
  });

  it('WHEN a non-Error is thrown THEN a generic reason is shown', () => {
    expect(failSubmit(filled, 'nope').error).toBe('Action failed');
  });

  it('WHEN the form is cancelled THEN reopening starts from empty fields', () => {
    const reopened = openPasswordForm(7);
    expect(reopened).toEqual({ instance: expect.any(Symbol), userId: 7, current: '', value: '', error: null, saving: false });
  });

  it('WHEN the same user is opened twice THEN each opening is its own form instance', () => {
    expect(openPasswordForm(7).instance).not.toBe(openPasswordForm(7).instance);
  });
});

describe('GIVEN a password change that is still pending', () => {
  /** A request the test settles by hand, so the form can change while it is in flight. */
  function deferredChange() {
    let settle!: { resolve: () => void; reject: (reason: Error) => void };
    const promise = new Promise<void>((resolve, reject) => {
      settle = { resolve, reject };
    });
    return { change: () => promise, settle };
  }

  const sent = { ...openPasswordForm(7), current: 'old-secret', value: 'new-secret', saving: true };

  it('WHEN the form is still shown and the server refuses THEN the refusal is shown in it', async () => {
    const { change, settle } = deferredChange();
    const pending = submitPasswordChange(sent, change);
    settle.reject(new Error('403 Forbidden: current password is incorrect'));
    const shown = settlePasswordChange(sent, sent, await pending);
    expect(shown.instance).toBe(sent.instance);
    expect(shown.error).toBe('403 Forbidden: current password is incorrect');
    expect(shown.saving).toBe(false);
  });

  it('WHEN the form was dismissed and the server then refuses THEN the form stays closed', async () => {
    const { change, settle } = deferredChange();
    const pending = submitPasswordChange(sent, change);
    const dismissed = closedPasswordForm;
    settle.reject(new Error('403 Forbidden: current password is incorrect'));
    expect(settlePasswordChange(dismissed, sent, await pending)).toBe(dismissed);
  });

  it('WHEN another form was opened meanwhile THEN a late refusal leaves the new form alone', async () => {
    const { change, settle } = deferredChange();
    const pending = submitPasswordChange(sent, change);
    const reopened = { ...openPasswordForm(7), value: 'typed-after-reopening' };
    settle.reject(new Error('403 Forbidden: current password is incorrect'));
    expect(settlePasswordChange(reopened, sent, await pending)).toBe(reopened);
  });

  it('WHEN another form was opened meanwhile THEN a late success does not close it', async () => {
    const { change, settle } = deferredChange();
    const pending = submitPasswordChange(sent, change);
    const other = openPasswordForm(8);
    settle.resolve();
    expect(settlePasswordChange(other, sent, await pending)).toBe(other);
  });
});
