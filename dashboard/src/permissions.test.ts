import { describe, expect, it } from 'vitest';
import { canDeleteAgents, canEditWebhooks, canManageGroups, canManageUsers, canOperate, isGlobal } from './permissions';
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

describe('GIVEN the agent delete and webhook helpers', () => {
  it('WHEN canDeleteAgents is asked THEN only admins qualify', () => {
    expect(rolesWhere(canDeleteAgents)).toEqual(['global_admin', 'group_admin']);
  });

  it('WHEN canEditWebhooks is asked THEN only the global admin qualifies', () => {
    expect(rolesWhere(canEditWebhooks)).toEqual(['global_admin']);
  });
});
