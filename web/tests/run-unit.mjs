// Bundle TypeScript unit tests with Vite's esbuild and run them with node:test.
// Usage: node tests/run-unit.mjs [filter]
import { build } from 'esbuild';
import { mkdtempSync, readdirSync, rmSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';
import { tmpdir } from 'node:os';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('.', import.meta.url));
const filter = process.argv[2] ?? '';
const files = [];
(function walk(dir) {
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) walk(path);
    else if (name.endsWith('.test.ts') && path.includes(filter)) files.push(path);
  }
})(root);
if (files.length === 0) {
  console.error('no unit tests matched');
  process.exit(1);
}
const out = mkdtempSync(join(tmpdir(), 'm33mu-web-unit-'));
try {
  await build({
    entryPoints: files, outdir: out, outbase: root, bundle: true, platform: 'node',
    format: 'esm', target: 'node20', outExtension: { '.js': '.mjs' }, sourcemap: 'inline',
    logLevel: 'warning', external: ['node:*']
  });
  const bundles = files.map((f) => join(out, relative(root, f)).replace(/\.ts$/, '.mjs'));
  const run = spawnSync(process.execPath, ['--enable-source-maps', '--test', ...bundles],
    { stdio: 'inherit', env: { ...process.env, M33MU_WEB_ROOT: join(root, '..') } });
  process.exitCode = run.status ?? 1;
} finally {
  rmSync(out, { recursive: true, force: true });
}
