// Bundle the worker entry with its engine and the TypeScript circuit solver.
// The Emscripten module stays a separate file next to the bundle.
// Usage: node scripts/bundle-web-worker.mjs <output-dir>
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';

const repo = fileURLToPath(new URL('..', import.meta.url));
const require = createRequire(join(repo, 'web', 'package.json'));
let esbuild;
try {
  esbuild = require('esbuild');
} catch {
  console.error('bundle-web-worker: esbuild missing; run `npm --prefix web ci` first');
  process.exit(1);
}
const out = process.argv[2];
if (!out) {
  console.error('usage: bundle-web-worker.mjs <output-dir>');
  process.exit(2);
}
await esbuild.build({
  entryPoints: [join(repo, 'web', 'worker', 'worker.mjs')],
  outfile: join(out, 'worker.mjs'), bundle: true, format: 'esm', target: 'es2022',
  platform: 'neutral', external: ['./m33mu_web_api.js'], logLevel: 'warning',
  legalComments: 'inline'
});
