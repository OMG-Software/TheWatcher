/**
 * Shown when data could not be loaded. This is not the same as "no data":
 * pages show it instead of their empty-state text. With `stale`, older data is
 * still on screen below the banner.
 */
export function LoadFailed({ message, onRetry, stale = false, what }: { message: string; onRetry?: () => void; stale?: boolean; what: string }) {
  return (
    <div className="banner error" role="alert">
      {stale ? `Could not refresh ${what}: ${message}. Showing the last data received.` : `Could not load ${what}: ${message}`}
      {onRetry ? <button className="text-button banner-action" onClick={onRetry} type="button">Retry</button> : null}
    </div>
  );
}
