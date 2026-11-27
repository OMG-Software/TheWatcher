import React from 'react';
import { Pause, Pencil, Play, Plus, SlidersHorizontal, Trash2, X } from 'lucide-react';
import { changeUserPassword, createGroup, createUser, deleteUser, disableUser, enableUser, updateUserAccess } from '../api';
import { ActionError } from '../components/ActionError';
import { ActionButton } from '../components/ActionButton';
import { ActionFailureContext, type RunAction } from '../hooks/useRunAction';
import type { GroupRecord, SessionInfo, UserRecord } from '../models';
import { canChangePassword, canEditMembership, canManageAccount, canManageGroups, canManageUsers, isGlobal } from '../permissions';
import { accountTypeLabel } from '../uiLogic';

const GROUP_ROLES: UserRecord['role'][] = ['group_admin', 'group_operator', 'group_viewer'];
const ALL_ROLES: UserRecord['role'][] = ['global_admin', 'global_operator', 'global_viewer', ...GROUP_ROLES];

export function UsersPage({
  busyAction,
  groups,
  runAction,
  session,
  users,
}: {
  busyAction: string | null;
  groups: GroupRecord[];
  runAction: RunAction;
  session: SessionInfo;
  users: UserRecord[];
}) {
  const [groupName, setGroupName] = React.useState('');
  const [username, setUsername] = React.useState('');
  const [password, setPassword] = React.useState('');
  const [role, setRole] = React.useState<UserRecord['role']>('group_viewer');
  const [groupId, setGroupId] = React.useState<number>(groups[0]?.group_id ?? 0);
  const [pwUserId, setPwUserId] = React.useState<number | null>(null);
  const [pwValue, setPwValue] = React.useState('');
  const [editUserId, setEditUserId] = React.useState<number | null>(null);
  const [editRole, setEditRole] = React.useState<UserRecord['role']>('group_viewer');
  const [editGroupIds, setEditGroupIds] = React.useState<number[]>([]);
  const { clear } = React.useContext(ActionFailureContext);
  const pwKey = `user:${pwUserId}:pw`;
  const pwBusy = busyAction === pwKey;
  // The password dialog cannot be dismissed while saving; dismissing drops its old failure.
  const closePw = () => {
    if (pwBusy) return;
    clear(pwKey);
    setPwUserId(null);
  };

  const admin = canManageUsers(session.role);
  const globalAccount = isGlobal(session.role);
  // The groups the signed-in account administers: every group the server lists for it.
  const ownGroupIds = groups.map((group) => group.group_id);
  const editUser = users.find((user) => user.user_id === editUserId) ?? null;
  const editKey = `user:${editUserId}:edit`;
  const editBusy = busyAction === editKey;
  // The edit dialog cannot be dismissed while saving; dismissing drops its old failure.
  const closeEdit = () => {
    if (editBusy) return;
    clear(editKey);
    setEditUserId(null);
  };
  const openEdit = (user: UserRecord) => {
    setEditUserId(user.user_id);
    setEditRole(user.role);
    // Only the groups this account can see are editable; the rest stay as they are.
    setEditGroupIds(user.group_ids.filter((id) => globalAccount || ownGroupIds.includes(id)));
  };
  const currentUser = users.find((u) => u.username === session.username);

  React.useEffect(() => {
    if (!globalAccount && groups.length > 0 && !groups.some((group) => group.group_id === groupId)) {
      setGroupId(groups[0].group_id);
    }
  }, [globalAccount, groupId, groups]);

  return (
    <main className="table-wrap management-wrap">
      {pwUserId !== null ? (
        <div className="modal-backdrop" role="presentation" onClick={closePw}>
          <div className="modal-panel" onClick={(e) => e.stopPropagation()}>
            <div className="modal-header">
              <h2>Change Password</h2>
              <button className="icon-button" disabled={pwBusy} onClick={closePw} aria-label="Close"><X size={14} /></button>
            </div>
            <label>
              New password
              <input
                type="password"
                autoFocus
                value={pwValue}
                onChange={(e) => setPwValue(e.target.value)}
              />
            </label>
            <ActionError actionKey={`user:${pwUserId}:pw`} />
            <div className="button-row modal-actions">
              <button className="text-button secondary" disabled={pwBusy} onClick={closePw}>Cancel</button>
              <button
                className="text-button"
                disabled={!pwValue || busyAction === `user:${pwUserId}:pw`}
                onClick={() => {
                  const id = pwUserId;
                  // Close and clear only once the server accepted the new password.
                  void runAction(`user:${id}:pw`, () => changeUserPassword(id, pwValue), { inline: true }).then((saved) => {
                    if (!saved) return;
                    setPwUserId(null);
                    setPwValue('');
                  });
                }}
              >
                Save
              </button>
            </div>
          </div>
        </div>
      ) : null}
      {editUser ? (() => {
        const roleEditable = canManageAccount(session.role, ownGroupIds, editUser);
        const visibleGroupIds = globalAccount ? groups.map((group) => group.group_id) : ownGroupIds;
        const initialGroupIds = editUser.group_ids.filter((id) => globalAccount || ownGroupIds.includes(id));
        const sameIds = (a: number[], b: number[]) => a.length === b.length && a.every((id) => b.includes(id));
        const roleChanged = roleEditable && editRole !== editUser.role;
        const groupsChanged = !sameIds(editGroupIds, initialGroupIds);
        return (
          <div className="modal-backdrop" onClick={closeEdit}>
            <div className="modal-panel" role="dialog" aria-label={`Edit ${editUser.username}`} onClick={(e) => e.stopPropagation()}>
              <div className="modal-header">
                <h2>Edit {editUser.username}</h2>
                <button className="icon-button" disabled={editBusy} onClick={closeEdit} aria-label="Close"><X size={14} /></button>
              </div>
              <label>
                Account type
                <select value={editRole} disabled={!roleEditable} onChange={(e) => setEditRole(e.target.value as UserRecord['role'])}>
                  {(globalAccount ? ALL_ROLES : GROUP_ROLES).map((role) => (
                    <option key={role} value={role}>{accountTypeLabel(role)}</option>
                  ))}
                  {!(globalAccount ? ALL_ROLES : GROUP_ROLES).includes(editUser.role) ? <option value={editUser.role}>{accountTypeLabel(editUser.role)}</option> : null}
                </select>
              </label>
              <fieldset>
                <legend>Groups</legend>
                {visibleGroupIds.map((id) => (
                  <label key={id} className="checkbox-label">
                    <input
                      type="checkbox"
                      checked={editGroupIds.includes(id)}
                      onChange={(e) => setEditGroupIds(e.target.checked ? [...editGroupIds, id] : editGroupIds.filter((g) => g !== id))}
                    />
                    {groups.find((group) => group.group_id === id)?.name ?? id}
                  </label>
                ))}
              </fieldset>
              <ActionError actionKey={editKey} />
              <div className="button-row modal-actions">
                <button className="text-button secondary" disabled={editBusy} onClick={closeEdit}>Cancel</button>
                <button
                  className="text-button"
                  disabled={editBusy || (!roleChanged && !groupsChanged)}
                  onClick={() => {
                    const id = editUser.user_id;
                    // Send only what changed, so a group admin never re-sends a role it may not set.
                    const changes: { role?: UserRecord['role']; group_ids?: number[] } = {};
                    if (roleChanged) changes.role = editRole;
                    if (groupsChanged) changes.group_ids = editGroupIds;
                    void runAction(editKey, () => updateUserAccess(id, changes), { inline: true }).then((saved) => {
                      if (saved) setEditUserId(null);
                    });
                  }}
                >
                  Save
                </button>
              </div>
            </div>
          </div>
        );
      })() : null}
      <div className="management-header"><h1>Users & Groups</h1></div>

      <ActionError actionKey="group:create" />
      <ActionError actionKey="user:create" />
      <div className="management-grid">
        {canManageGroups(session.role) ? <form
          className="inline-form"
          onSubmit={(event) => {
            event.preventDefault();
            if (!groupName.trim()) return;
            void runAction('group:create', () => createGroup(groupName.trim()), { inline: true }).then((saved) => {
              if (saved) setGroupName('');
            });
          }}
        >
          <input aria-label="Group name" placeholder="Group name" value={groupName} onChange={(event) => setGroupName(event.target.value)} />
          <ActionButton busy={busyAction === 'group:create'} icon={<Plus size={14} />} label="Group" onClick={() => undefined} type="submit" />
        </form> : null}
        {admin ? <form
          className="inline-form user-form"
          onSubmit={(event) => {
            event.preventDefault();
            if (!username.trim() || !password) return;
            void runAction('user:create', () => createUser(username.trim(), password, role, groupId ? [groupId] : []), { inline: true }).then((saved) => {
              if (!saved) return;
              setUsername('');
              setPassword('');
            });
          }}
        >
          <input aria-label="Username" placeholder="Username" value={username} onChange={(event) => setUsername(event.target.value)} />
          <input aria-label="Password" placeholder="Password" type="password" value={password} onChange={(event) => setPassword(event.target.value)} />
          <select aria-label="Account type" value={role} onChange={(event) => setRole(event.target.value as UserRecord['role'])}>
            {globalAccount ? <option value="global_admin">{accountTypeLabel('global_admin')}</option> : null}
            {globalAccount ? <option value="global_operator">{accountTypeLabel('global_operator')}</option> : null}
            {globalAccount ? <option value="global_viewer">{accountTypeLabel('global_viewer')}</option> : null}
            <option value="group_admin">{accountTypeLabel('group_admin')}</option>
            <option value="group_operator">{accountTypeLabel('group_operator')}</option>
            <option value="group_viewer">{accountTypeLabel('group_viewer')}</option>
          </select>
          <select aria-label="Group" value={groupId} onChange={(event) => setGroupId(Number(event.target.value))}>
            {globalAccount ? <option value={0}>No group</option> : null}
            {groups.map((group) => (
              <option key={group.group_id} value={group.group_id}>{group.name}</option>
            ))}
          </select>
          <ActionButton busy={busyAction === 'user:create'} icon={<Plus size={14} />} label="User" onClick={() => undefined} type="submit" />
        </form> : null}
      </div>
      <div className="table-container">
        <table className="management-table">
          <thead><tr><th>User</th><th>Type</th><th>Groups</th><th>State</th><th>Actions</th></tr></thead>
          <tbody>{users.map((user) => {
            const canChangePw = canChangePassword(session.role, currentUser?.user_id === user.user_id);
            const canEdit = canEditMembership(session.role, ownGroupIds, user);
            const canManage = canManageAccount(session.role, ownGroupIds, user);

            return (
              <tr key={user.user_id} className={user.disabled ? 'row-archived' : ''}>
                <td>{user.username}</td>
                <td>{accountTypeLabel(user.role)}</td>
                <td>{user.group_ids.map((id) => groups.find((group) => group.group_id === id)?.name ?? id).join(', ')}</td>
                <td>{user.disabled ? 'Disabled' : 'Active'}</td>
                <td>
                  <div className="button-row">
                    {canChangePw ? (
                      <ActionButton
                        busy={busyAction === `user:${user.user_id}:pw`}
                        icon={<SlidersHorizontal size={14} />}
                        label="Password"
                        onClick={() => { setPwUserId(user.user_id); setPwValue(''); }}
                      />
                    ) : null}
                    {canEdit ? (
                      <ActionButton
                        busy={editBusy && editUserId === user.user_id}
                        icon={<Pencil size={14} />}
                        label="Edit"
                        onClick={() => openEdit(user)}
                      />
                    ) : null}
                    {canManage ? (
                      user.disabled ? (

                        <ActionButton
                          busy={busyAction === `user:${user.user_id}:enable`}
                          icon={<Play size={14} />}
                          label="Enable"
                          onClick={() => runAction(`user:${user.user_id}:enable`, () => enableUser(user.user_id))}
                        />
                      ) : (
                        <ActionButton
                          busy={busyAction === `user:${user.user_id}:disable`}
                          icon={<Pause size={14} />}
                          label="Disable"
                          onClick={() => runAction(`user:${user.user_id}:disable`, () => disableUser(user.user_id))}
                        />
                      )
                    ) : null}
                    {canManage ? (
                      <ActionButton
                        busy={busyAction === `user:${user.user_id}:delete`}
                        danger
                        icon={<Trash2 size={14} />}
                        label="Delete"
                        onClick={() => runAction(`user:${user.user_id}:delete`, () => deleteUser(user.user_id))}
                      />
                    ) : null}
                  </div>
                </td>
              </tr>
            );
          })}</tbody>
        </table>
      </div>
    </main>
  );
}

