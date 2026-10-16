trigger: when a rebuild takes minutes because one line changed, or the image is far larger than the code
tags: #docker
weight: 65
verified: 2026-10
reference: https://docs.docker.com/build/cache/
sources: model knowledge 2026-10; pending review against Kane, Docker: Up and Running 3ed

An image is a stack of layers, one per build instruction, and each is cached
against the instruction *and* the content of everything it copies. The whole game
is ordering instructions so the expensive, rarely-changing ones come first.

- **Copy the dependency manifest before the source**: `COPY package.json .` then
  install, then `COPY . .`. The layer that runs the install stays cached while the
  code churns. Reversing those two lines rebuilds dependencies on every edit.
- **`.dockerignore` is part of the build**, not housekeeping: `.git`, `node_modules`,
  `build/`, test fixtures. A large context is slow to send and silently changes
  what `COPY . .` contains.
- **One `RUN` per logical step, chained with `&&` and cleaned in the same layer**
  — a `rm` in a later instruction removes nothing from the image, because the
  earlier layer still holds it.
- **`--cache-from` and BuildKit's inline cache** make a CI build reuse what a
  previous CI run produced; without it, every pipeline starts from scratch.
- **Rebuild on the manifest, not on hope**: a lockfile change must invalidate the
  install layer, and `--no-cache` is a debugging tool, not a policy.
- **Read the layer sizes** (`docker history <image>`) rather than guessing what is
  big; the answer is usually a build tool that should have been multi-stage.

## Run it

```run
# manual: needs a docker daemon; the shape is
# docker build -t app . && docker history --no-trunc app | head
docker --version
```
