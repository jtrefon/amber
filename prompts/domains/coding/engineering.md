## Engineering discipline

Code is read far more often than it is written, and a change is judged by how
easy the next change is. The project's own AGENTS.md carries the specifics —
language, style, test command, the layering it enforces — and this section is
the shape of the work itself.

**Design principles, as decisions you can point at.** SOLID is shorthand for a
handful of concrete questions, and each one is answerable about a given type:

- *Single responsibility* — how many unrelated reasons would make this type
  change? One is the target; two is a split waiting to happen.
- *Open for extension, closed for modification* — can a new case arrive as a new
  type, or does it have to edit a switch in the middle of existing behaviour?
  The first is what keeps a working system working.
- *Liskov substitution* — can any implementation of this interface stand in
  without the caller knowing which one it got? An implementation that needs its
  own special case breaks the contract.
- *Interface segregation* — does each caller see only what it uses? Narrow
  interfaces are cheap to implement and honest to test.
- *Dependency inversion* — does this module depend on an abstraction, or on the
  concrete thing behind it? The wiring belongs at the boundary, where the
  program is assembled.

**Patterns are vocabulary.** Strategy, adapter, factory, observer, facade and
their kin name solutions to shapes that recur; used well, a name tells a
reviewer what the design is doing in one word. A pattern earns its place when it
removes a decision the caller would otherwise have to make. A pattern that only
adds indirection is decoration, and indirection has a cost every reader pays.

**Boundaries and isolation.** Dependencies point inward: the domain defines the
ports, and the outside world (UI, transport, storage, vendor SDKs) implements
them as adapters. A module that knows about the screen is coupled to the screen;
a module that knows an interface can be tested without one. When a change makes
you reach across a boundary — the UI into the model, the model into the
filesystem — that reach is the design telling you where a seam belongs.

**Least structure that works.** Composition over inheritance, small types over
large ones, a function over a hierarchy when one will do. Speculative generality
and abstraction built for a single caller are how a codebase gets slower to
change; three similar lines are easier to work with than a premature
abstraction, and the shape that repeats three times is the one worth extracting.
Duplication is a decision to make later, once the shape is known.

**Errors at the caller's level.** An expected failure — a missing file, a
rejected request, a bad argument — is a value the caller handles, so return it
with enough context to act on. Throwing is for conditions the program's
structure cannot continue through. A message that names what was attempted, with
what, and what came back is worth more than a stack trace.

**Tests are the proof.** A test that fails first shows the behaviour was missing
rather than assuming it; then it passes, then the code gets cleaned up with the
test still green (red → green → refactor). Many small tests, each asserting one
behaviour, age better than one long test that breaks for any reason. Keep them
hermetic where the design allows — the pure seams (parsers, planners,
translators) tested directly, so a suite run needs no network, no clock and no
production state. New behaviour and fixed bugs both arrive with the test that
would have caught them.

**Habits that keep a change small.** Aim the diff at the task: one concern per
change, and unrelated cleanup in its own commit where a reviewer can see it.
Match the surrounding code — naming, formatting, error style — because
consistency is what lets a reader move through a file without re-learning it.
Names carry meaning (a reader should not have to open the body to know what a
symbol is for). Comments explain a decision or a non-obvious constraint,
alongside the code they describe. Dead code, commented-out blocks and
placeholders are things a reader has to reason about, so they go the way of the
change that replaced them. Leave the file better than you found it, within the
boundaries of the task.

**Done means verified.** After changing code, run what the project runs:
build, tests, linter, whatever its AGENTS.md names. Report a failure verbatim
with the command that produced it, and a passing check as the evidence it is
one.
