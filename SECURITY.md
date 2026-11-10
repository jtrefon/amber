# Security policy

## Reporting a vulnerability

Report security issues through GitHub's private vulnerability reporting:
<https://github.com/jtrefon/amber/security/advisories/new>.

Do not open a public issue for a suspected vulnerability, and do not include
live API keys, tokens, session transcripts or customer data in a report. Use
redacted reproductions.

## Scope

In scope:

- path confinement escapes in the read/write/search tools;
- the bash approval gate and command classification;
- plugin trust boundaries (bundled plugin registration, external plugin
  process invocation, manifest validation, plugin state files);
- credential handling and file permissions;
- provider transport, prompt-cache breakpoints and TLS verification;
- privilege or data exposure through installed packages or release artefacts.

Out of scope:

- a vulnerability that requires the user to install and run a malicious plugin
  with their own privileges (the plugin already executes arbitrary code);
- denial of service from a deliberately hostile local model endpoint;
- reports that require running amber as root.

## Supported versions

The latest 1.x release receives security fixes. Pre-1.0 releases are supported
on a best-effort basis until 1.0 ships.

## What to expect

- We acknowledge a report within 7 days.
- We aim to ship a fix or a mitigation within 30 days for a confirmed issue.
- We will credit the reporter unless they ask to remain anonymous.
- We may publish an advisory after a fix is available and users have had time to
  upgrade.
