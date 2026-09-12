// Local preview server for the amber website.
// Serves website/dist at both "/" and the GitHub Pages base path "/amber/",
// so the browser preview proxy (which serves at its own root) can resolve the
// base-prefixed asset URLs emitted by add-base.mjs.
import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { join, extname, normalize } from 'node:path';

const ROOT = new URL('../dist/', import.meta.url).pathname;
const PORT = 4321;

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.webp': 'image/webp',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.ico': 'image/x-icon',
  '.woff2': 'font/woff2',
  '.xml': 'application/xml',
  '.json': 'application/json',
};

async function resolve(pathname) {
  let rel = pathname.replace(/^\/amber(?=\/|$)/, '') || '/';
  rel = normalize(decodeURIComponent(rel)).replace(/^(\.\.[/\\])+/, '');
  let file = join(ROOT, rel);
  try {
    const s = await stat(file);
    if (s.isDirectory()) file = join(file, 'index.html');
  } catch {
    if (!extname(file)) file = join(file, 'index.html');
  }
  return file;
}

createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://localhost:${PORT}`);
    const file = await resolve(url.pathname);
    const body = await readFile(file);
    res.writeHead(200, { 'content-type': TYPES[extname(file)] || 'application/octet-stream' });
    res.end(body);
  } catch {
    res.writeHead(404, { 'content-type': 'text/plain' });
    res.end('404');
  }
}).listen(PORT, () => console.log(`amber preview on http://localhost:${PORT}/amber/`));
