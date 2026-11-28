import { describe, expect, it } from 'vitest';
import { canChangePassword, canEditMembership, canManageAccount, canManageGroups, canManageUsers, canOperate, isGlobal } from './permissions';
import type { UserRole } from './models';

const ALL_ROLES: UserRole[] = ['global_admin', 'global_operator', 'global_viewer', 'group_admin', 'group_operator', 'group_viewer'];

const rolesWhere = (predicate: (role: UserRole) => boolean) => ALL_ROLES.filter(predicate);

describe('GIVEN the role helpers in permissions', () => {
  it('WHEN isGlobal is asked THEN only the global roles qualify', () => {
    expect(rolesWhere(isGlobal)).toEqual(['global_admin', 'global_operator', 'global_viewer']);
  });

  it('WHEN canOperate is asked THEN admins and operators qualify but viewers do not', () => {
    expect(rolesWhere(canOperate)).toEqual(['global_admin', 'global_operator', 'group_admin', 'group_operator']);
  });

  it('WHEN canManageUsers is asked THEN only admins qualify', () => {
    expect(rolesWhere(canManageUsers)).toEqual(['global_admin', 'group_admin']);
  });

  it('WHEN canManageGroups is asked THEN only the global admin qualifies', () => {
    expect(rolesWhere(canManageGroups)).toEqual(['global_admin']);
  });
});

describe('GIVEN the account management rules', () => {
  const viewerInA = { built_in: false, group_ids: [1], role: 'group_viewer' as UserRole, user_id: 5 };
  const viewerInAB = { ...viewerInA, group_ids: [1, 2] };
  const viewerInB = { ...viewerInA, group_ids: [2] };
  const globalOperator = { ...viewerInA, role: 'global_operator' as UserRole };
  const builtIn = { ...viewerInA, built_in: true, role: 'global_admin' as UserRole };

  it('WHEN a global admin manages users THEN everyone but the built-in user qualifies', () => {
    expect(canManageAccount('global_admin', [], viewerInB)).toBe(true);
    expect(canManageAccount('global_admin', [], globalOperator)).toBe(true);
    expect(canManageAccount('global_admin', [], builtIn)).toBe(false);
  });

  it('WHEN a group admin manages users THEN only group-scoped users wholly in their groups qualify', () => {
    expect(canManageAccount('group_admin', [1], viewerInA)).toBe(true);
    expect(canManageAccount('group_admin', [1], viewerInAB)).toBe(false);
    expect(canManageAccount('group_admin', [1], viewerInB)).toBe(false);
    expect(canManageAccount('group_admin', [1], globalOperator)).toBe(false);
  });

  it('WHEN a group admin edits membership THEN a user shared with another group still qualifies', () => {
    expect(canEditMembership('group_admin', [1], viewerInAB)).toBe(true);
    expect(canEditMembership('group_admin', [1], viewerInB)).toBe(false);
    expect(canEditMembership('group_admin', [1], globalOperator)).toBe(false);
  });

  it('WHEN the session is not an admin THEN nothing qualifies', () => {
    expect(canManageAccount('group_operator', [1], viewerInA)).toBe(false);
    expect(canEditMembership('global_operator', [1], viewerInA)).toBe(false);
  });

  it('WHEN passwords are changed THEN only a global admin or the user themselves qualify', () => {
    expect(canChangePassword('global_admin', false)).toBe(true);
    expect(canChangePassword('group_admin', false)).toBe(false);
    expect(canChangePassword('group_viewer', true)).toBe(true);
  });
});
