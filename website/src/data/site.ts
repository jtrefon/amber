// Single source of truth for site-wide constants.
// The version is read from the repo's version.txt at build time, the same
// file ./configure reads to stamp include/agent/version.h and the release
// workflow syncs to the git tag. This ensures the website, the binary, and
// the release all agree on a single version without manual duplication.

import { readFileSync } from 'node:fs';
import { join } from 'node:path';

function readVersion(): string {
  // Astro build runs from website/, so version.txt is one level up.
  // Try cwd-relative first (works for both astro build and astro dev).
  const candidates = [
    join(process.cwd(), '..', 'version.txt'),  // website/ -> repo root
    join(process.cwd(), 'version.txt'),         // repo root (fallback)
  ];
  for (const p of candidates) {
    try {
      return readFileSync(p, 'utf8').trim();
    } catch {
      // try next
    }
  }
  return 'unknown';
}

const repo = 'https://github.com/jtrefon/amber';

export const site = {
  version: readVersion(),
  repo,
  homebrewTap: 'jtrefon/homebrew-amber',
  homebrewFormula: 'amber-agent',
  license: 'Apache 2.0',
  platforms: ['Linux', 'macOS'],
  description:
    'Amber is an open-source C++17 AI agent runtime and CLI harness with an extensible plugin architecture, multi-provider support, MCP, sub-agents and terminal clients for Linux and macOS.',
  title: 'amber | native C++ AI agent runtime',
  // Deep links into GitHub's new-issue flow, with the matching Issue Form
  // pre-selected. The forms live in .github/ISSUE_TEMPLATE/*.yml.
  issues: {
    newBug: `${repo}/issues/new?template=bug_report.yml`,
    newFeature: `${repo}/issues/new?template=feature_request.yml`,
    list: `${repo}/issues`,
    contributing: `${repo}/blob/main/CONTRIBUTING.md`,
  },
};
