import React from 'react';
import { Pause, Play, Plus, SlidersHorizontal, Trash2 } from 'lucide-react';
import { changeUserPassword, createGroup, createUser, deleteUser, disableUser, enableUser } from '../api';
import { ActionError } from '../components/ActionError';
import { ActionButton } from '../components/ActionButton';
import { ConfirmDialog } from '../components/ConfirmDialog';
import { Modal } from '../components/Modal';
import { ActionFailureContext, type RunAction } from '../hooks/useRunAction';
import type { GroupRecord, SessionInfo, UserRecord } from '../models';
import { canManageGroups, canManageUsers, isGlobal } from '../permissions';
import { accountTypeLabel } from '../uiLogic';

export function UsersPage({
  isBusy,
  groups,
  runAction,
  session,
  users,
}: {
  isBusy: (key: string) => boolean;
  groups: GroupRecord[];
  runAction: RunAction;
  session: SessionInfo;
  users: UserRecord[];
}) {
  const [groupName, setGroupName] = React.useState('');
  const [username, setUsername] = React.useState('');
  const [password, setPassword] = React.useState('');
  const [role, setRole] = React.useState<UserRecord['role']>('group_viewer');
  const [groupChoice, setGroupId] = React.useState<number>(groups[0]?.group_id ?? 0);
  const [pwUserId, setPwUserId] = React.useState<number | null>(null);
  const [pwValue, setPwValue] = React.useState('');
  const [deletingUser, setDeletingUser] = React.useState<UserRecord | null>(null);
  const { clear } = React.useContext(ActionFailureContext);
  const pwKey = `user:${pwUserId}:pw`;
  const pwBusy = isBusy(pwKey);
  // The password dialog cannot be dismissed while saving; dismissing drops its old failure.
  const closePw = () => {
    if (pwBusy) return;
    clear(pwKey);
    setPwUserId(null);
  };

  const admin = canManageUsers(session.role);
  const globalAdmin = session.role === 'global_admin';
  const globalAccount = isGlobal(session.role);
  const currentUser = users.find((u) => u.username === session.username);
  // A group user must pick one of their groups, so fall back to the first when the choice is gone.
  const groupId = !globalAccount && groups.length > 0 && !groups.some((group) => group.group_id === groupChoice) ? groups[0].group_id : groupChoice;

  return (
    <main className="table-wrap management-wrap">
      {pwUserId !== null ? (
        <Modal dismissible={!pwBusy} onClose={closePw} title="Change Password">
            <label>
              New password
              <input
                type="password"
                autoComplete="new-password"
                data-autofocus
                value={pwValue}
                onChange={(e) => setPwValue(e.target.value)}
              />
            </label>
            <ActionError actionKey={`user:${pwUserId}:pw`} />
            <div className="button-row modal-actions">
              <button className="text-button secondary" disabled={pwBusy} onClick={closePw}>Cancel</button>
              <button
                className="text-button"
                disabled={!pwValue || isBusy(`user:${pwUserId}:pw`)}
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
        </Modal>
      ) : null}
      {deletingUser ? (
        <ConfirmDialog
          busy={isBusy(`user:${deletingUser.user_id}:delete`)}
          confirmLabel="Delete user"
          danger
          errorKey={`user:${deletingUser.user_id}:delete`}
          message={<>Delete user <strong>{deletingUser.username}</strong>? Their sessions end and the account cannot be recovered.</>}
          onCancel={() => setDeletingUser(null)}
          onConfirm={() => runAction(`user:${deletingUser.user_id}:delete`, () => deleteUser(deletingUser.user_id), { inline: true })}
          title="Delete user"
        />
      ) : null}
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
          <ActionButton busy={isBusy('group:create')} icon={<Plus size={14} />} label="Group" onClick={() => undefined} type="submit" />
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
          <input aria-label="Password" autoComplete="new-password" placeholder="Password" type="password" value={password} onChange={(event) => setPassword(event.target.value)} />
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
          <ActionButton busy={isBusy('user:create')} icon={<Plus size={14} />} label="User" onClick={() => undefined} type="submit" />
        </form> : null}
      </div>
      <div className="table-container">
        <table className="management-table">
          <thead><tr><th>User</th><th>Type</th><th>Groups</th><th>State</th><th>Actions</th></tr></thead>
          <tbody>{users.map((user) => {
            const canChangePw = admin || currentUser?.user_id === user.user_id;
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
                        busy={isBusy(`user:${user.user_id}:pw`)}
                        icon={<SlidersHorizontal size={14} />}
                        label="Password"
                        onClick={() => { setPwUserId(user.user_id); setPwValue(''); }}
                      />
                    ) : null}
                    {globalAdmin && !user.built_in ? (
                      user.disabled ? (
                        <ActionButton
                          busy={isBusy(`user:${user.user_id}:enable`)}
                          icon={<Play size={14} />}
                          label="Enable"
                          onClick={() => runAction(`user:${user.user_id}:enable`, () => enableUser(user.user_id))}
                        />
                      ) : (
                        <ActionButton
                          busy={isBusy(`user:${user.user_id}:disable`)}
                          icon={<Pause size={14} />}
                          label="Disable"
                          onClick={() => runAction(`user:${user.user_id}:disable`, () => disableUser(user.user_id))}
                        />
                      )
                    ) : null}
                    {globalAdmin && !user.built_in ? (
                      <ActionButton
                        busy={isBusy(`user:${user.user_id}:delete`)}
                        danger
                        icon={<Trash2 size={14} />}
                        label="Delete"
                        onClick={() => setDeletingUser(user)}
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

