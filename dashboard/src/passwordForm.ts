import type { UserRole } from './models';

/** State of the Change Password modal. `userId` is null while it is closed. */
export interface PasswordFormState {
  userId: number | null;
  current: string;
  value: string;
  error: string | null;
  saving: boolean;
}

export const closedPasswordForm: PasswordFormState = { userId: null, current: '', value: '', error: null, saving: false };

export function openPasswordForm(userId: number): PasswordFormState {
  return { ...closedPasswordForm, userId };
}

/**
 * Own account: any role. Someone else's: only a global admin, because the API
 * refuses every other role with 403. Until the own account is known (`undefined`)
 * only the global admin gets the button.
 */
export function canChangePassword(role: UserRole, sessionUserId: number | undefined, targetUserId: number): boolean {
  return role === 'global_admin' || sessionUserId === targetUserId;
}

/** Changing your own password must prove the current one; an admin reset of someone else does not. */
export function needsCurrentPassword(form: PasswordFormState, sessionUserId: number | undefined): boolean {
  return form.userId !== null && form.userId === sessionUserId;
}

/** A refused change keeps the modal open and the new password, shows the reason, and clears the (wrong) current password. */
export function failSubmit(form: PasswordFormState, reason: unknown): PasswordFormState {
  return { ...form, current: '', error: reason instanceof Error ? reason.message : 'Action failed', saving: false };
}

/** Runs the change; resolves the closed form on success and the failed form (reason shown) on rejection. */
export async function submitPasswordChange(form: PasswordFormState, change: () => Promise<void>): Promise<PasswordFormState> {
  try {
    await change();
    return closedPasswordForm;
  } catch (err) {
    return failSubmit(form, err);
  }
}
