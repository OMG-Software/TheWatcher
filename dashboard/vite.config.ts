import { readFileSync } from 'node:fs';
import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

// The dashboard's own version, from package.json, injected at build time.
const { version } = JSON.parse(readFileSync(new URL('./package.json', import.meta.url), 'utf8')) as { version: string };

export default defineConfig({
  define: { __APP_VERSION__: JSON.stringify(version) },
  plugins: [react()],
  server: {
    port: 5173,
    strictPort: false,
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:8080',
        changeOrigin: true,
      },
    },
  },
  test: {
    // Collect only the TypeScript test sources, never compiled twins.
    include: ['src/**/*.test.{ts,tsx}'],
    // UI tests render React into a DOM; the pure-logic tests do not mind it.
    environment: 'jsdom',
    setupFiles: ['src/test-setup.ts'],
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
});
