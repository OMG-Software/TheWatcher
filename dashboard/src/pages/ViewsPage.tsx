import React from 'react';
import { Check, Search, SlidersHorizontal, Trash2, X } from 'lucide-react';
import { createView, deleteView, updateView } from '../api';
import { ActionButton } from '../components/ActionButton';
import type { RunAction } from '../hooks/useRunAction';
import type { DashboardAgent, GroupRecord, SessionInfo, ViewRecord } from '../models';
import { isGlobal } from '../permissions';

export function ViewsPage({
  agents,
  busyAction,
  operator,
  runAction,
  session,
  views,
  onOpenView,
  groups,
}: {
  agents: DashboardAgent[];
  busyAction: string | null;
  operator: boolean;
  runAction: RunAction;
  session: SessionInfo;
  views: ViewRecord[];
  onOpenView: (viewId: number) => void;
  groups: GroupRecord[];
}) {
  const [newName, setNewName] = React.useState('');
  const [newPublic, setNewPublic] = React.useState(false);
  const [newGroupId, setNewGroupId] = React.useState<number>(groups[0]?.group_id ?? 0);
  const [newAgentIds, setNewAgentIds] = React.useState<string[]>([]);
  const [editId, setEditId] = React.useState<number | null>(null);
  const [editName, setEditName] = React.useState('');
  const [editPublic, setEditPublic] = React.useState(false);
  const [editGroupId, setEditGroupId] = React.useState(0);
  const [editAgentIds, setEditAgentIds] = React.useState<string[]>([]);
  const globalAccount = isGlobal(session.role);

  React.useEffect(() => {
    if (!globalAccount && groups.length > 0 && !groups.some((group) => group.group_id === newGroupId)) {
      setNewGroupId(groups[0].group_id);
    }
  }, [globalAccount, groups, newGroupId]);

  React.useEffect(() => {
    if (!globalAccount && editId !== null && groups.length > 0 && !groups.some((group) => group.group_id === editGroupId)) {
      setEditGroupId(groups[0].group_id);
    }
  }, [editGroupId, editId, globalAccount, groups]);

  const startEdit = (v: ViewRecord) => {
    setEditId(v.view_id);
    setEditName(v.name);
    setEditPublic(v.is_public);
    setEditGroupId(v.group_id);
    setEditAgentIds([...v.agent_ids]);
  };

  const canEdit = (v: ViewRecord) => operator && (v.owner_username === session.username || isGlobal(session.role));

  return (
    <main className="table-wrap">
      <div className="table-container">
        <table>
          <thead>
            <tr>
              <th>Name</th>
              <th>Owner</th>
              <th>Agents</th>
              <th>Visibility</th>
              <th>Group</th>
              <th>Created</th>
              <th />
            </tr>
          </thead>
          <tbody>
            {views.map((v) => (
              <tr key={v.view_id}>
                {editId === v.view_id ? (
                  <>
                    <td colSpan={5}>
                      <div className="form-grid" style={{ display: 'flex', gap: '0.5rem', flexWrap: 'wrap', alignItems: 'center' }}>
                        <input value={editName} placeholder="View name" onChange={(e) => setEditName(e.target.value)} />
                        <label style={{ display: 'flex', gap: '0.25rem', alignItems: 'center' }}>
                          <input type="checkbox" checked={editPublic} onChange={(e) => setEditPublic(e.target.checked)} />
                          Public
                        </label>
                        <select multiple size={Math.min(agents.length, 6)} style={{ minWidth: '14rem' }}
                          value={editAgentIds}
                          onChange={(e) => setEditAgentIds(Array.from(e.target.selectedOptions, (o) => o.value))}>
                          {agents.map((a) => <option key={a.id} value={a.id}>{a.name}</option>)}
                        </select>
                        <select value={editGroupId} onChange={(e) => setEditGroupId(Number(e.target.value))}>
                          {globalAccount ? <option value={0}>No group</option> : null}
                          {groups.map((group) => <option key={group.group_id} value={group.group_id}>{group.name}</option>)}
                        </select>
                      </div>
                    </td>
                    <td>{new Date(v.created_at).toLocaleDateString()}</td>
                    <td>
                      <ActionButton busy={busyAction === `view:save:${v.view_id}`} label="Save" icon={<Check size={14} />}
                        onClick={() => void runAction(`view:save:${v.view_id}`, () => updateView(v.view_id, editName, editAgentIds, editPublic, editGroupId).then(() => {})).then(() => setEditId(null))} />
                      <ActionButton busy={false} label="Cancel" icon={<X size={14} />} onClick={() => setEditId(null)} />
                    </td>
                  </>
                ) : (
                  <>
                    <td><strong>{v.name}</strong></td>
                    <td>{v.owner_username}</td>
                    <td>{v.agent_ids.length} agent{v.agent_ids.length !== 1 ? 's' : ''}</td>
                    <td>{v.is_public ? 'Public' : 'Private'}</td>
                    <td>{groups.find((group) => group.group_id === v.group_id)?.name ?? 'No group'}</td>
                    <td>{new Date(v.created_at).toLocaleDateString()}</td>
                    <td>
                      <ActionButton busy={false} label="Open" icon={<Search size={14} />} onClick={() => onOpenView(v.view_id)} />
                      {canEdit(v) ? <ActionButton busy={false} label="Edit" icon={<SlidersHorizontal size={14} />} onClick={() => startEdit(v)} /> : null}
                      {canEdit(v) ? (
                        <ActionButton busy={busyAction === `view:delete:${v.view_id}`} danger label="Delete"
                          icon={<Trash2 size={14} />}
                          onClick={() => void runAction(`view:delete:${v.view_id}`, () => deleteView(v.view_id))} />
                      ) : null}
                    </td>
                  </>
                )}
              </tr>
            ))}
            {views.length === 0 ? (
              <tr><td colSpan={7} style={{ textAlign: 'center', padding: '1rem' }}>No views yet. Create one below.</td></tr>
            ) : null}
          </tbody>
        </table>
      </div>

      {operator ? (
        <div className="table-container" style={{ marginTop: '1rem' }}>
          <form
            onSubmit={(e) => {
              e.preventDefault();
              void runAction('view:create', () => createView(newName, newAgentIds, newPublic, newGroupId).then(() => {})).then(() => {
                setNewName('');
                setNewPublic(false);
                setNewAgentIds([]);
              });
            }}
            style={{ padding: '1rem', display: 'flex', gap: '0.75rem', flexWrap: 'wrap', alignItems: 'flex-start' }}
          >
            <input required value={newName} placeholder="New view name" onChange={(e) => setNewName(e.target.value)} style={{ minWidth: '12rem' }} />
            <label style={{ display: 'flex', gap: '0.25rem', alignItems: 'center' }}>
              <input type="checkbox" checked={newPublic} onChange={(e) => setNewPublic(e.target.checked)} />
              Public
            </label>
            <select multiple size={Math.min(agents.length, 6)} style={{ minWidth: '14rem' }}
              value={newAgentIds}
              onChange={(e) => setNewAgentIds(Array.from(e.target.selectedOptions, (o) => o.value))}>
              {agents.map((a) => <option key={a.id} value={a.id}>{a.name}</option>)}
            </select>
            <select value={newGroupId} onChange={(e) => setNewGroupId(Number(e.target.value))}>
              {globalAccount ? <option value={0}>No group</option> : null}
              {groups.map((group) => <option key={group.group_id} value={group.group_id}>{group.name}</option>)}
            </select>
            <button className="text-button" type="submit" disabled={busyAction === 'view:create' || !newName}>
              {busyAction === 'view:create' ? '...' : 'Create View'}
            </button>
          </form>
        </div>
      ) : null}
    </main>
  );
}

