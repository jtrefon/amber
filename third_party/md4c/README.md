# md4c (vendored)

Markdown parser for C — <https://github.com/mity/md4c>.

- **Version:** upstream `master` snapshot, fetched 2026-07-18.
- **Upstream revision:** UNRECORDED. The fetch came from `master`, not a tag or
  commit, so there is no revision to re-fetch or diff against. A known gap, not an
  oversight to be papered over with a plausible-looking SHA.
- **Integrity** (sha256 of the vendored files -- this pins *what we have*, not
  which upstream commit produced it):

  | file | sha256 |
  |---|---|
  | `md4c.c` | `c4a0b8aa5495e4d97eb9fb112b8aa4bb147666416737569f239e7ae1be8abf2c` |
  | `md4c.h` | `3e6940ebfc14c197552fc506f68d7685a950a503d7a256002c2740d4d2b62deb` |

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
Re-fetch `md4c.c` and `md4c.h` from a **tag or a commit -- not `master`**, then
record all three of:

1. the upstream revision (`git rev-parse HEAD` in your md4c clone),
2. the fetch date,
3. the new sha256 pair in the table above.

`make check` (build-hygiene **P11**) fails if the vendored files no longer match
that table, or if the table stops documenting them. Dependabot cannot see vendored
code, which is the whole reason the checksums are here.
