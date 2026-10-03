import { readdir, readFile } from 'node:fs/promises';
import { dirname, join, relative } from 'node:path';
import http from 'node:http';
import https from 'node:https';

const base = new URL(process.env.SMOKE_BASE_URL ?? 'http://127.0.0.1:4173/amber/');

function request(path) {
  return new Promise((resolve, reject) => {
    const url = new URL(path, base);
    const client = url.protocol === 'https:' ? https : http;
    const req = client.get(url, (response) => {
      response.resume();
      if (response.statusCode >= 400) {
        reject(new Error(`${response.statusCode} ${url}`));
        return;
      }
      resolve();
    });

    req.setTimeout(10_000, () => req.destroy(new Error(`timeout ${url}`)));
    req.on('error', reject);
  });
}

async function htmlFiles(directory) {
  const entries = await readdir(directory, { withFileTypes: true });
  const files = [];
  for (const entry of entries) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) files.push(...(await htmlFiles(path)));
    else if (entry.name.endsWith('.html')) files.push(path);
  }
  return files;
}

const dist = process.env.SMOKE_DIST ?? new URL('../dist/', import.meta.url).pathname;

// Routes come from the artifact, not a hardcoded list: every index.html under
// dist/ is a route, so the check stays complete as pages are added or removed
// with no edit here. (The previous list pinned nine routes and had already
// drifted: the build produces twenty-one pages.)
const pages = await htmlFiles(dist);
const routes = pages
  .filter((file) => file.endsWith('index.html'))
  .map((file) => {
    const dir = dirname(relative(dist, file));
    return dir === '.' ? '' : `${dir}/`;
  });

for (const route of routes) await request(route);

const links = new Set();
for (const file of pages) {
  const html = await readFile(file, 'utf8');
  for (const match of html.matchAll(/(?:href|src)="(\/amber\/[^"#?]*)"/g)) {
    links.add(match[1]);
  }
}
for (const link of links) await request(link);

console.log(`website smoke OK: ${routes.length} routes and ${links.size} internal assets`);
