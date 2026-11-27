import { vi } from 'vitest';
import type { UserRole } from '../models';

/** What a fake endpoint answers: a status and a JSON body. */
export interface FakeReply {
  status: number;
  body: unknown;
}

export const ok = (body: unknown = { ok: true }): FakeReply => ({ status: 200, body });
export const fail = (status: number, error: string): FakeReply => ({ status, body: { error } });

type Handler = FakeReply | (() => FakeReply | Promise<FakeReply>);

/**
 * A fake REST server for UI tests. Routes are keyed "METHOD /path"; anything
 * not listed answers 200 with an empty list. `calls` records every request.
 */
export class FakeApi {
  readonly calls: { method: string; path: string; body: unknown }[] = [];
  private readonly routes = new Map<string, Handler>();

  constructor(role: UserRole = 'global_admin') {
    this.on('GET /api/session', ok({ username: 'tester', role }));
    this.on('GET /api/agents', ok([agentRecord('agent-1', 'web-01')]));
    this.on('GET /api/metrics', ok([]));
  }

  on(route: string, handler: Handler): this {
    this.routes.set(route, handler);
    return this;
  }

  install() {
    const fetchMock = vi.fn(async (url: string, init?: RequestInit) => {
      const method = init?.method ?? 'GET';
      const path = String(url);
      this.calls.push({ method, path, body: init?.body ? JSON.parse(String(init.body)) : undefined });
      const handler = this.routes.get(`${method} ${path}`);
      const reply = typeof handler === 'function' ? await handler() : (handler ?? ok([]));
      return new Response(JSON.stringify(reply.body), { status: reply.status });
    });
    vi.stubGlobal('fetch', fetchMock);
    return fetchMock;
  }

  count(route: string): number {
    return this.calls.filter((call) => `${call.method} ${call.path}` === route).length;
  }
}

export function agentRecord(id: string, hostname: string) {
  return {
    agent_id: id,
    hostname,
    platform: 'linux',
    curve_public_key_z85: '',
    approved: true,
    rejected: false,
    connected: true,
    maintenance: false,
    maintenance_reason: '',
    maintenance_until: 0,
    collection_interval: 10,
    heartbeat_interval: 10,
    process_limit: 5,
    first_seen: 1,
    last_seen: Date.now(),
    description: '',
    runbook_markdown: '',
    group_ids: [],
  };
}

/** A promise the test settles by hand, to hold a request open and control response order. */
export function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((res) => {
    resolve = res;
  });
  return { promise, resolve };
}
