// add-base.mjs — GitHub Pages project sites serve under /amber/.
// Astro's `base` config only prefixes generated assets (_astro/, fonts/);
// component-authored root-relative links (href="/install") would resolve to
// the org root and 404. This rewrites them in the built HTML to /amber/...
import { readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const dist = join(import.meta.dirname, '..', 'dist');
const base = '/amber';
const skipPrefixes = [base + '/', '/_astro/', '/fonts/'];

const htmlFiles = [];
function walk(dir) {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const path = join(dir, entry.name);
    if (entry.isDirectory()) walk(path);
    else if (entry.name.endsWith('.html')) htmlFiles.push(path);
  }
}
walk(dist);

let rewritten = 0;
for (const path of htmlFiles) {
  const html = readFileSync(path, 'utf8');
  const out = html.replace(/(href|src)="\/([^"]*)"/g, (match, attr, path) => {
    const rest = '/' + path;
    if (skipPrefixes.some((p) => rest.startsWith(p))) return match;
    rewritten++;
    return `${attr}="${base}${rest}"`;
  });
  if (out !== html) writeFileSync(path, out);
}

console.log(`add-base: rewrote root-relative links in ${htmlFiles.length} pages (${rewritten} links)`);
