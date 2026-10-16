trigger: when a compose file grows past three services, or a service starts before its database is ready
tags: #dockercompose
weight: 70
verified: 2026-10
reference: https://docs.docker.com/compose/compose-file/
sources: model knowledge 2026-10; pending review against Kane, Docker: Up and Running 3ed

Compose describes a set of containers that belong together; the file is where a
developer's understanding of the system is written down, or hidden.

- **`depends_on` orders *startup*, not *readiness*.** By default it waits for the
  container to start, not for the service inside to accept connections — which is
  why an app crashes on a cold start and works on a warm one. Use
  `depends_on: { db: { condition: service_healthy } }` with a real `healthcheck`.
- **Healthchecks are the contract**: `test: ["CMD", "pg_isready", "-U", "app"]`,
  an `interval`, a `retries` count, and a `start_period` long enough for a first
  boot. Without them, "ready" means "the process exists".
- **Profiles** (`profiles: [debug]`) keep the optional services — a mail catcher,
  a debugger sidecar — out of the default `up` while staying in the file.
- **Environment precedence is a ladder**: `environment:` in the file beats
  `env_file:`, both beat the image's `ENV`, and the shell environment passed with
  `--env` beats everything at run time. Compose prints the winner with
  `docker compose config`.
- **Named volumes for state, bind mounts for code**, and know which is which: a
  named volume survives `down` and holds a stale schema; a bind mount hides what
  the image built (a `node_modules` from the host at a different architecture).
- **`docker compose down -v` is the reset**, and the `-v` is the part that deletes
  the data — say so out loud before running it on anything that matters.

## Run it

```run
# manual: needs the compose CLI and a daemon; the shape is
# docker compose config   # resolves variables, profiles and overrides without starting anything
docker compose version
```
