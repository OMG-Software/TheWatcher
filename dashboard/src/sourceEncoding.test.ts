// @vitest-environment node
/// <reference types="node" />
// The production build (vite build) refuses a source file that is not valid
// UTF-8, while the test runner quietly decodes it with replacement characters.
// This test makes the unit-test run fail the same way the build does. It runs
// in the node environment because it only reads files (no DOM needed).
import { mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const srcDir = fileURLToPath(new URL('.', import.meta.url));

// Binary assets (images, fonts, media) are not decoded as text by the build,
// so they are the only files skipped. Everything else is checked, including
// extensions not used today (.svg, .md, files with none), so a new file type
// cannot slip past this test.
const binaryAsset = /\.(png|jpe?g|gif|webp|avif|bmp|ico|woff2?|ttf|otf|eot|mp3|mp4|webm|wasm)$/i;

function sourceFiles(dir: string): string[] {
  return readdirSync(dir, { withFileTypes: true }).flatMap((entry) => {
    const path = join(dir, entry.name);
    if (entry.isDirectory()) return sourceFiles(path);
    return binaryAsset.test(entry.name) ? [] : [path];
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

describe('GIVEN a directory with text files of any extension and a binary image', () => {
  it('WHEN the files to check are listed THEN every text file is included and the image is not', () => {
    const dir = mkdtempSync(join(tmpdir(), 'tw-utf8-'));
    try {
      mkdirSync(join(dir, 'nested'));
      for (const name of ['a.ts', 'icon.svg', 'notes.md', 'nested/LICENSE', 'logo.png']) {
        writeFileSync(join(dir, name), 'x');
      }

      const listed = sourceFiles(dir)
        .map((file) => relative(dir, file).split(sep).join('/'))
        .sort();

      expect(listed).toEqual(['a.ts', 'icon.svg', 'nested/LICENSE', 'notes.md']);
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  });
});

describe('GIVEN bytes that are not valid UTF-8', () => {
  it('WHEN they are checked THEN they are reported as invalid', () => {
    expect(isValidUtf8(new Uint8Array([0x61, 0xff, 0x62]))).toBe(false);
    expect(isValidUtf8(new TextEncoder().encode('café'))).toBe(true);
  });
});
