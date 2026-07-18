# md4c (vendored)

Markdown parser for C — <https://github.com/mity/md4c>.

- **Version:** upstream `master` snapshot, fetched 2026-07-18.
- **License:** MIT (see `md4c.c` / `md4c.h` headers).
- **Files:** `md4c.c`, `md4c.h` only. No other dependencies; compiled as a
  single translation unit (`-DMD4C_USE_UTF8`).
- **Why vendored:** the project builds fully offline (no FetchContent /
  submodule / package manager). md4c is a single-file, dependency-free,
  CommonMark-compliant SAX parser whose callbacks map 1:1 onto our
  `rich::Line` terminal renderer.

## Build integration
Compiled to `third_party/md4c/md4c.o` and linked into the TUI binary (and
unit tests). Enabled flags: `MD_FLAG_TABLES | MD_FLAG_TASKLISTS |
MD_FLAG_STRIKETHROUGH`.

## Updating
Re-fetch `src/md4c.c` and `src/md4c.h` from the upstream tag you want, update
the version date above, and re-run `make`.
