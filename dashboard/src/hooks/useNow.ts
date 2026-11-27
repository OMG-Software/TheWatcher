import React from 'react';

/**
 * The current time in milliseconds, refreshed on an interval. Time is read here, in state, and
 * not while a component renders, so renders stay pure and tests can drive the clock.
 */
export function useNow(intervalMs = 30_000): number {
  const [now, setNow] = React.useState(() => Date.now());
  React.useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), intervalMs);
    return () => window.clearInterval(timer);
  }, [intervalMs]);
  return now;
}
