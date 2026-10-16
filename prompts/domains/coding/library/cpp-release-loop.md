trigger: when a release is cut, or a build works in CI and fails for a user
tags: #cpp #packaging #build
weight: 40
verified: 2026-10
reference: https://cmake.org/cmake/help/latest/module/CPack.html

The release loop is build, install, smoke-test the installed artefact, publish —
and the third step is the one that is usually missing.

- **Install to a scratch prefix and run something against it** in CI: build →
  `cmake --install --prefix`, then compile a two-line consumer with the installed
  headers and libraries. That single job catches missing headers, wrong
  `INSTALL_RPATH`, and a library that only worked in its build tree.
- **Version in exactly one place** (`project(foo VERSION 1.2.3)`) and generate the
  header from it; a second hard-coded version string is a release that forgets to
  bump.
- **Tag the source, not the artefact**: `git tag -a v1.2.3` at the commit the
  artefacts came from, so "what is in the release" has an answer.
- **Ship the ABI promise**: SOVERSION major matches the compatibility rule, and a
  breaking change means a new major, not a new patch.
- **Artifacts are content-addressed by their hash**: publish the sha256 next to
  the tarball and, where the ecosystem has one, a signature (cosign, GPG). A
  release nobody can verify is a tarball with a version in the name.
- **A release is a rehearsal of the emergency**: if the rollback is not documented
  and tested, the first incident becomes the first rehearsal.
- **Notes come from the commits** (conventional or scoped messages, grouped), not
  from memory at release time.

## Run it

```run
# manual: needs a real release pipeline; the shape is
# build -> install to a prefix -> build a consumer against that prefix -> publish
echo "smoke test the installed artefact, not the build tree"
```
