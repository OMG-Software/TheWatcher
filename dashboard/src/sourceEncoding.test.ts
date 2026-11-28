// @vitest-environment node
/// <reference types="node" />
// The production build (vite build) refuses a source file that is not valid
// UTF-8, while the test runner quietly decodes it with replacement characters.
// This test makes the unit-test run fail the same way the build does. It runs
// in the node environment because it only reads files (no DOM needed).
import { readdirSync, readFileSync } from 'node:fs';
import { join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const srcDir = fileURLToPath(new URL('.', import.meta.url));

function sourceFiles(dir: string): string[] {
  return readdirSync(dir, { withFileTypes: true }).flatMap((entry) => {
    const path = join(dir, entry.name);
    if (entry.isDirectory()) return sourceFiles(path);
    return /\.(ts|tsx|css|json|html)$/.test(entry.name) ? [path] : [];
  });
}

function isValidUtf8(bytes: Uint8Array): boolean {
  try {
    new TextDecoder('utf-8', { fatal: true }).decode(bytes);
    return true;
  } catch {
    return false;
  }
}

describe('GIVEN the dashboard source files', () => {
  it('WHEN each file is decoded THEN every one is valid UTF-8', () => {
    const files = sourceFiles(srcDir);
    const invalid = files.filter((file) => !isValidUtf8(readFileSync(file))).map((file) => relative(srcDir, file));

    expect(files.length).toBeGreaterThan(0);
    expect(invalid).toEqual([]);
  });
});
