import { act, renderHook } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { useRunAction } from './useRunAction';

describe('GIVEN a mutation that succeeds', () => {
  it('WHEN the refresh afterwards fails THEN the action still reports success and the refresh failure is a page-level banner', async () => {
    const refresh = vi.fn().mockRejectedValue(new Error('store unavailable'));
    const action = vi.fn().mockResolvedValue(undefined);
    const { result } = renderHook(() => useRunAction(refresh));

    let saved: boolean | undefined;
    await act(async () => {
      saved = await result.current.runAction('silence:create', action, { inline: true });
    });

    expect(saved).toBe(true);
    expect(action).toHaveBeenCalledTimes(1);
    expect(result.current.failure?.inline).toBe(false);
    expect(result.current.failure?.message).toMatch(/could not be refreshed.*store unavailable/);
  });
});

describe('GIVEN a mutation that fails', () => {
  it('WHEN it runs THEN the action reports failure, skips the refresh and keeps the failure under its key', async () => {
    const refresh = vi.fn().mockResolvedValue(undefined);
    const { result } = renderHook(() => useRunAction(refresh));

    let saved: boolean | undefined;
    await act(async () => {
      saved = await result.current.runAction('x', () => Promise.reject(new Error('nope')), { inline: true });
    });

    expect(saved).toBe(false);
    expect(refresh).not.toHaveBeenCalled();
    expect(result.current.failure).toEqual({ key: 'x', message: 'nope', inline: true });
  });
});

describe('GIVEN a failure shown by a dialog', () => {
  const failed = async () => {
    const hook = renderHook(() => useRunAction(vi.fn().mockResolvedValue(undefined)));
    await act(async () => {
      await hook.result.current.runAction('alert:1:ack', () => Promise.reject(new Error('nope')), { inline: true });
    });
    return hook;
  };

  it('WHEN that dialog clears its own key THEN the failure is gone', async () => {
    const { result } = await failed();

    act(() => result.current.clearFailure('alert:1:ack'));

    expect(result.current.failure).toBeNull();
  });

  it('WHEN a different key is cleared THEN the failure stays', async () => {
    const { result } = await failed();

    act(() => result.current.clearFailure('alert:2:ack'));

    expect(result.current.failure?.key).toBe('alert:1:ack');
  });
});
