trigger: when a value is copied, moved or cloned and the reason is not obvious — say who owns it
tags: #cpp #rust #ownership
verified: 2026-10
sources: model knowledge 2026-10; not yet reviewed against the guides
weight: 55
reference: none

Ownership is not bookkeeping added after the design; it is most of the design.
When a copy appears, ask which of these it actually is:

- **Transfer** — the caller is done with the value: move it (C++ `std::move`,
  Rust by-value) and make the moved-from state explicit.
- **Share** — two owners genuinely need it: in C++, a `shared_ptr` says so; in
  Rust, `Arc`. A shared pointer where a reference would do is a lifetime hidden
  from the reader.
- **Duplicate on purpose** — the copy is the semantics (a value type, a
  snapshot), and saying that out loud prevents someone "optimising" it away.

If none of the three holds, the boundary is in the wrong place: pass a
`const&` / borrow, or move the work to where the data lives.
