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

/** Only admins (global or group) may delete an agent, which is also how an approved agent is revoked. */
export function canDeleteAgents(role: UserRole): boolean {
  return role === 'global_admin' || role === 'group_admin';
}

/** Only the global admin may read or change webhook URLs; other global roles see them masked. */
export function canEditWebhooks(role: UserRole): boolean {
  return role === 'global_admin';
}

/** Only the global admin may create and manage groups. */
export function canManageGroups(role: UserRole): boolean {
  return role === 'global_admin';
}
