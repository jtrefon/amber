#!/usr/bin/env node
// Dependency audit gate.
//
// Fails on any high or critical advisory, except those named in
// audit-exceptions.json. An exception must carry BOTH a reason and an expiry,
// and the gate fails once one lapses — so an exception cannot quietly outlive
// the situation that justified it. Any new advisory fails immediately.
//
// Usage: npm run audit

import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const BLOCKING = new Set(['high', 'critical']);
const here = dirname(fileURLToPath(import.meta.url));

function loadExceptions() {
  const path = join(here, '..', 'audit-exceptions.json');
  return JSON.parse(readFileSync(path, 'utf8')).exceptions ?? [];
}

// `npm audit` exits non-zero when it finds anything; the report is still on
// stdout, so the throw path has to hand back what was written.
function runAudit() {
  const opts = { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] };
  try {
    return JSON.parse(execFileSync('npm', ['audit', '--json'], opts));
  } catch (err) {
    if (err.stdout) return JSON.parse(err.stdout);
    throw err;
  }
}

// The advisory objects in a report, deduplicated by id. Entries in `via` that
// are plain strings are package names (a package flagged because something it
// depends on is affected), not advisories.
function advisories(report) {
  const found = new Map();
  for (const [name, entry] of Object.entries(report.vulnerabilities ?? {})) {
    for (const via of entry.via ?? []) {
      if (typeof via !== 'object') continue;
      const id = (via.url ?? '').split('/').pop();
      if (!id || found.has(id)) continue;
      found.set(id, {
        id,
        package: name,
        severity: via.severity,
        title: via.title,
        fixAvailable: entry.fixAvailable,
      });
    }
  }
  return [...found.values()];
}

const exceptions = loadExceptions();
const byId = new Map(exceptions.map((e) => [e.id, e]));

for (const e of exceptions) {
  if (!e.reason || !e.expires) {
    console.error(`audit: exception ${e.id} must carry both a reason and an expiry`);
    process.exit(2);
  }
}

const today = new Date().toISOString().slice(0, 10);
const failures = [];
let allowed = 0;

for (const a of advisories(runAudit())) {
  if (!BLOCKING.has(a.severity)) continue;
  const exception = byId.get(a.id);
  if (!exception) {
    failures.push(`  ${a.id} (${a.severity}) in ${a.package}: ${a.title}`);
    continue;
  }
  if (today > exception.expires) {
    failures.push(
      `  ${a.id} (${a.severity}) in ${a.package}: exception expired ${exception.expires} — re-review it`,
    );
    continue;
  }
  allowed += 1;
  console.warn(`audit: allowing ${a.id} in ${a.package} until ${exception.expires}`);
  console.warn(`audit:   ${exception.reason}`);
  if (a.fixAvailable) {
    console.warn(`audit:   npm offers: ${JSON.stringify(a.fixAvailable)}`);
  }
}

if (failures.length) {
  console.error('audit: blocking advisories');
  console.error(failures.join('\n'));
  process.exit(1);
}
console.log(`audit: no blocking advisories (${allowed} allowed by exception)`);
