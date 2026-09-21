import { readdir, readFile } from 'node:fs/promises';
import { join, relative } from 'node:path';

const base = new URL(process.env.SMOKE_BASE_URL ?? 'http://127.0.0.1:4173/amber/');
const routes = [
  '',
  'download/',
  'install/',
  'benchmarks/',
  'architecture/',
  'architecture/diagrams/',
  'plugins/',
  'manual/',
  'report/',
];

async function request(path) {
  const url = new URL(path, base);
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`${response.status} ${url}`);
  }
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
for (const route of routes) await request(route);

const links = new Set();
for (const file of await htmlFiles(dist)) {
  const html = await readFile(file, 'utf8');
  for (const match of html.matchAll(/(?:href|src)="(\/amber\/[^"#?]*)"/g)) {
    links.add(match[1]);
  }
}
for (const link of links) await request(link);

console.log(`website smoke OK: ${routes.length} routes and ${links.size} internal assets`);
