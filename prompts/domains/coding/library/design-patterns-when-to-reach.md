trigger: when a shape repeats in the code and every call site knows which variant it holds
tags: #patterns #design
also: design patterns, singleton, strategy, factory, adapter, observer
weight: 55
verified: 2026-10
reference: https://refactoring.guru/design-patterns/criticism
sources: Ousterhout, A Philosophy of Software Design 2ed §19.5; Kerievsky, Refactoring to Patterns ch. 3 (Patterns Happy; Do Patterns Make Code More Complex?); Casciaro & Mammino, Node.js Design Patterns 4ed ch. 7; Despoudis, TypeScript 5 Design Patterns 2ed ch. 3 & 10; Danilov, Refactoring with C++ ch. 4; https://en.wikipedia.org/wiki/Design_Patterns

A pattern is vocabulary for a shape that is already in the code, not a plan for one
that might be. The question is not "which pattern fits?" but "which axis of variation
is already hurting, and who should stop knowing about it?" Three questions decide it;
if none of them has an answer, the abstraction is a cost with no payer:

- **Can you name the second implementation?** One implementation behind an interface
  is a promise nobody has collected. Two real variants — or one now plus a second the
  code already branches on — is what turns a seam into a saving. Designing for a third
  variant nobody asked for is *Speculative Generality*, and it is the smell most
  pattern-shaped damage arrives as.
- **What does the discrimination cost today?** A `switch (type)` in the one place that
  owns the data *is* the design: readable, greppable, and exhaustiveness-checked by the
  compiler. The pattern's real justification is a discrimination that reappears in five
  call sites, each of which must be taught about every new variant.
- **Where does a new variant land?** The payoff is that adding one touches a
  composition root and nothing else. If adding one still edits an enum, a factory and a
  switch in three files, the pattern was bolted on rather than used.

When it pays: the variance is a named axis, more than one implementation exists today,
and the callers should not care which one they hold. Everything the catalogue does to
reach that is one idea — program to an interface; favour composition over inheritance —
and the GoF wrote it down before writing the patterns.

When it is ceremony: one implementation, one call site, or a hierarchy where a function
value would do. Much of the catalogue is a workaround for languages that lacked
closures: Norvig showed 16 of the 23 collapse in a dynamic language, and in C++ a
`std::function` parameter does what Strategy and much of Template Method were invented
for. Gamma said in 2009 he would have dropped Singleton.

The trap is the pattern as the deliverable — Kerievsky's *patterns-happy* — code shaped
to use something just learned, then harder to read for flexibility nobody asked for.
Two habits keep it honest: treat the pattern as a destination reached by refactoring
when the second case appears, not as an up-front skeleton (refactor to, towards, and
away from patterns; the goal is a better design, not a pattern), and price the seam —
indirection costs a virtual call and a lifetime to reason about, and "we might need it"
is not a measurement.

## Run it

```run
python3 - <<'PY'
# What is left of four catalogue rows once the language has closures.
def chain(handlers, value):                 # chain of responsibility / middleware
    for handler in handlers:
        value = handler(value)
    return value

strategy = lambda rows, key: sum(key(row) for row in rows)   # Strategy: a parameter
listeners = []                                               # Observer: a list
command = lambda: "saved"                                    # Command: a closure
listeners.append(command)

assert strategy([{"p": 2}, {"p": 3}], lambda r: r["p"]) == 5
assert chain([lambda x: x + 1, lambda x: x * 2], 3) == 8
assert listeners and listeners[0]() == "saved"
print("strategy, command, observer, chain: four functions, no declarations")
PY
```
