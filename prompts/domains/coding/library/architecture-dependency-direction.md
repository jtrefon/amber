trigger: when a handler, a repository type or a framework header starts appearing inside the domain — decide which way the dependency is allowed to point, and where the seam sits
tags: #architecture #design
also: hexagonal, layering, ports and adapters, dependency direction
weight: 50
verified: 2026-10
reference: https://alistair.cockburn.us/hexagonal-architecture
sources: Cockburn, "Hexagonal Architecture" (2005); Martin, "The Clean Architecture" (2012); Fowler, bliki "PresentationDomainDataLayering" (2015); Khononov, Learning Domain-Driven Design ch. 8; Gavrilin, Software Architecture with C++ 2ed ch. 2; FreeWheel, Cloud-Native Application Architecture ch. 2; Head First Software Architecture ch. 6

Layered, hexagonal, onion and clean are one drawing with a different arrow.
Layered stacks presentation over domain over data access, and the domain
imports the persistence client — the arrow runs outward, from policy to detail.
Ports and adapters (hexagonal, and the same move in onion and clean) inverts
that one relationship: the core declares interfaces, the outside implements
them, and every source dependency points inward. Onion adds rings and clean
names its circles; the arrow is the argument, and the test of the design never
changes:

**Can the domain be compiled and tested with no framework, no database and no
HTTP?** If the domain's object file needs a driver header, a JSON library or a
request type to compile, the arrow points the wrong way. Audit per file:
nothing inward may name anything outward — not a type, not a data format. A row
struct, a framework's DTO, an ORM entity, a clock or an ID generator are
outward details, so they belong behind a port the core defines.

What the wrong direction looks like once it has settled in:

- The domain suite needs a database, a container or a server to be running: the
  adapters are load-bearing at test time rather than interchangeable.
- One new field wants edits in the same commit across controller, service,
  mapper and schema — a single change smeared over every layer.
- A second consumer (CLI, batch job, another service) cannot reuse the core
  because the only entry point is the HTTP handler.
- The framework's vocabulary is in the core's signatures: `Request`, `Session`,
  `DbSet`, `sqlx::Pool`, a page model.

The trade is honest: inverting the arrow buys substitutability and a testable
core at the price of one indirection per boundary — an interface, a
translation, a wiring point in the composition root. A thin CRUD screen over
one table does not earn that; the seam belongs where change is expected (the
store, the UI, the vendor SDK), not on every boundary at once. Choosing the
direction is the decision; keeping it visible in the build is what makes it
hold.

## Run it

```run
# manual: needs the project's own build; the shape is
#   cmake --build build --target domain-core    # must not need a DB driver or an HTTP library
#   ctest --test-dir build -R domain            # and must pass with no server running
```
