trigger: when the runtime image carries a compiler, a package manager and 900 MB of nothing
tags: #docker
weight: 60
verified: 2026-10
reference: https://docs.docker.com/build/building/multi-stage/
sources: model knowledge 2026-10; pending review against Kane, Docker: Up and Running 3ed

Multi-stage builds separate "what it takes to build" from "what it takes to run",
and the second is almost always smaller than anyone expects.

- **Name the stages** (`FROM golang:1.22 AS build` … `FROM gcr.io/distroless/static`)
  and copy artefacts across: `COPY --from=build /out/app /app`. Nothing else
  crosses.
- **Static binaries go in `scratch` or distroless**: no shell, no package manager,
  no CVEs from a base image nobody looks at. Distroless still gives you CA
  certificates and a non-root user; `scratch` gives you nothing at all, which
  breaks TLS unless the certs are copied.
- **Dynamic runtimes need their libraries**: a glibc-linked binary in
  `alpine` fails with "no such file or directory" because the interpreter it was
  linked against is not there. Check with `ldd`, and either link statically
  (`CGO_ENABLED=0`, `-static`) or use a matching base.
- **Run as a user**: `USER 65532:65532` (or `nobody`) so a container escape is not
  root on the host, and so a mounted volume is not written as root.
- **Pin the base by digest** (`FROM node:20-alpine@sha256:…`) when reproducibility
  matters — a tag is a moving pointer, a digest is the image.
- **The build stage is where the toolchain lives**: delete nothing, it is not in
  the final image; keep the cache warm instead.

## Run it

```run
# manual: needs a docker daemon; the shape is
# docker build -t app . && docker image inspect app --format '{{.Size}}'
docker --version
```
