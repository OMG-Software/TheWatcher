import '@testing-library/jest-dom/vitest';
import { cleanup } from '@testing-library/react';
import { afterEach } from 'vitest';
import { installDialogShim } from './testing/dialog';

// jsdom cannot open a native <dialog> as a modal; see testing/dialog.ts.
installDialogShim();

// Unmount rendered trees between tests so they cannot leak state into each other.
afterEach(() => {
  cleanup();
});
