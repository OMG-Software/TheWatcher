import type { UserRole } from './models';

// Role helpers. The server enforces every permission; these only decide which
// controls the dashboard shows.

/** Global accounts see every group; group accounts only see their own. */
export function isGlobal(role: UserRole): boolean {
  return role.startsWith('global_');
}

/** Operators and admins (global or group) may run actions; viewers may not. */
export function canOperate(role: UserRole): boolean {
  return role === 'global_admin' || role === 'global_operator' || role === 'group_admin' || role === 'group_operator';
}

/** Only admins may manage user accounts. */
export function canManageUsers(role: UserRole): boolean {
  return role === 'global_admin' || role === 'group_admin';
}

/** Only the global admin may create and manage groups. */
export function canManageGroups(role: UserRole): boolean {
  return role === 'global_admin';
}

/** The part of a user record the account rules look at. */
export interface ManagedUser {
  built_in: boolean;
  group_ids: number[];
  role: UserRole;
  user_id: number;
}

const sharesGroup = (a: number[], b: number[]) => a.some((id) => b.includes(id));

/**
 * May the session change this user's membership of its own groups? A global
 * admin may for anyone but the built-in user; a group admin only for
 * group-scoped users who share one of their groups.
 */
export function canEditMembership(role: UserRole, ownGroupIds: number[], target: ManagedUser): boolean {
  if (target.built_in) return false;
  if (role === 'global_admin') return true;
  return role === 'group_admin' && !isGlobal(target.role) && sharesGroup(ownGroupIds, target.group_ids);
}

/**
 * May the session act on the whole account (role, disable, enable, delete)?
 * Those reach every group the user is in, so a group admin needs all of them
 * to be their own.
 */
export function canManageAccount(role: UserRole, ownGroupIds: number[], target: ManagedUser): boolean {
  if (!canEditMembership(role, ownGroupIds, target)) return false;
  return role === 'global_admin' || target.group_ids.every((id) => ownGroupIds.includes(id));
}

/** Only a global admin or the user themselves may change a password (the server's rule). */
export function canChangePassword(role: UserRole, isSelf: boolean): boolean {
  return role === 'global_admin' || isSelf;
}
