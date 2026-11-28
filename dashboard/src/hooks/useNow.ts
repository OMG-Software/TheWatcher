import React from 'react';

/**
 * The current time in milliseconds, refreshed on an interval (5 s by default, the dashboard's poll interval, so an expiry shows within one poll). Time is read here, in state, and
 * not while a component renders, so renders stay pure and tests can drive the clock.
 */
export function useNow(intervalMs = 5000): number {
  const [now, setNow] = React.useState(() => Date.now());
  React.useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), intervalMs);
    return () => window.clearInterval(timer);
  }, [intervalMs]);
  return now;
}
