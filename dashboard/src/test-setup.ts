import { cleanup } from '@testing-library/react';
import { afterEach } from 'vitest';

// Unmount rendered trees between tests so they cannot leak state into each other.
afterEach(() => {
  cleanup();
});
