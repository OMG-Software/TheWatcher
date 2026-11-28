import React from 'react';
import { Pause, Play, Plus, SlidersHorizontal, Trash2, X } from 'lucide-react';
import { changeUserPassword, createGroup, createUser, deleteUser, disableUser, enableUser } from '../api';
import { ActionButton } from '../components/ActionButton';
import type { RunAction } from '../hooks/useRunAction';
import type { GroupRecord, SessionInfo, UserRecord } from '../models';
import { canManageGroups, canManageUsers, isGlobal } from '../permissions';
import {
  canChangePassword,
  closedPasswordForm,
  needsCurrentPassword,
  openPasswordForm,
  settlePasswordChange,
  submitPasswordChange,
  type PasswordFormState,
} from '../passwordForm';
import { accountTypeLabel } from '../uiLogic';

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
  const [pwForm, setPwForm] = React.useState<PasswordFormState>(closedPasswordForm);

  const admin = canManageUsers(session.role);
  const globalAdmin = session.role === 'global_admin';
  const globalAccount = isGlobal(session.role);
  const currentUser = users.find((u) => u.username === session.username);
  // Changing your own password (any role) must prove the current one; an admin reset of someone else does not.
  const changingOwnPassword = needsCurrentPassword(pwForm, currentUser?.user_id);

  React.useEffect(() => {
    if (!globalAccount && groups.length > 0 && !groups.some((group) => group.group_id === groupId)) {
      setGroupId(groups[0].group_id);
    }
  }, [globalAccount, groupId, groups]);

  return (
    <main className="table-wrap management-wrap">
      {pwForm.userId !== null ? (
        <div className="modal-backdrop" onClick={() => setPwForm(closedPasswordForm)}>
          <div className="modal-panel" onClick={(e) => e.stopPropagation()}>
            <div className="modal-header">
              <h2>Change Password</h2>
              <button className="icon-button" onClick={() => setPwForm(closedPasswordForm)}><X size={14} /></button>
            </div>
            {changingOwnPassword ? (
              <label>
                Current password
                <input
                  type="password"
                  autoComplete="current-password"
                  autoFocus
                  disabled={pwForm.saving}
                  value={pwForm.current}
                  onChange={(e) => setPwForm({ ...pwForm, current: e.target.value })}
                />
              </label>
            ) : null}
            <label>
              New password
              <input
                type="password"
                autoComplete="new-password"
                autoFocus={!changingOwnPassword}
                // Locked while saving: the answer replaces the form with the one that was sent.
                disabled={pwForm.saving}
                value={pwForm.value}
                onChange={(e) => setPwForm({ ...pwForm, value: e.target.value })}
              />
            </label>
            {pwForm.error ? <p className="form-error" role="alert">{pwForm.error}</p> : null}
            <div className="button-row modal-actions">
              <button className="text-button secondary" onClick={() => setPwForm(closedPasswordForm)}>Cancel</button>
              <button
                className="text-button"
                disabled={pwForm.saving || !pwForm.value || (changingOwnPassword && !pwForm.current)}
                onClick={() => {
                  const id = pwForm.userId;
                  if (id === null) return;
                  const sent = { ...pwForm, error: null, saving: true };
                  const current = changingOwnPassword ? sent.current : undefined;
                  setPwForm(sent);
                  // The modal stays open until the server accepts the change; a refusal is shown inside it. The answer is
                  // applied only if this opening is still shown, so dismissing the modal meanwhile is final.
                  void submitPasswordChange(sent, () => changeUserPassword(id, sent.value, current)).then((result) =>
                    setPwForm((shown) => settlePasswordChange(shown, sent, result)),
                  );
                }}
              >
                Save
              </button>
            </div>
          </div>
        </div>
      ) : null}
      <div className="management-header"><h1>Users & Groups</h1></div>
      <div className="management-grid">
        {canManageGroups(session.role) ? <form
          className="inline-form"
          onSubmit={(event) => {
            event.preventDefault();
            if (!groupName.trim()) return;
            void runAction('group:create', () => createGroup(groupName.trim())).then(() => setGroupName(''));
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
            void runAction('user:create', () => createUser(username.trim(), password, role, groupId ? [groupId] : [])).then(() => {
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
            const canChangePw = canChangePassword(session.role, currentUser?.user_id, user.user_id);
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
                        onClick={() => setPwForm(openPasswordForm(user.user_id))}
                      />
                    ) : null}
                    {globalAdmin && !user.built_in ? (
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
                    {globalAdmin && !user.built_in ? (
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

