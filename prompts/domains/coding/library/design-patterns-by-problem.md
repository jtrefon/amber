trigger: when the symptom is known — a variant, a call to wrap, an object to build — but not the shape's name
tags: #patterns #design
also: design patterns, Gang of Four, GoF, pattern
weight: 55
verified: 2026-10
reference: https://refactoring.guru/design-patterns/catalog
sources: Despoudis, TypeScript 5 Design Patterns 2ed ch. 3-4 ("When to use" sections) and ch. 10 (class overuse); Casciaro & Mammino, Node.js Design Patterns 4ed ch. 7-9 (Factory, Singleton to DI, Proxy/Decorator boundary, Adapter, Strategy, State, Middleware); Ayeva, Mastering Python Design Patterns 3ed ch. 3 (should you use Singleton?); Danilov, Refactoring with C++ ch. 4; https://refactoring.guru/design-patterns/catalog

Read the symptom first; the name is how you tell someone else what you did, not a
decision procedure. The rows are the recurring symptoms, the shape that answers each,
and the form it usually takes when the language already has the mechanism:

| symptom | shape | what to actually write |
|---|---|---|
| vary an algorithm without touching callers | **Strategy** | a callable parameter (`std::function`, a lambda, a function pointer); a class only when variants own state |
| add behaviour around a call, in layers | **Decorator** / **Chain of Responsibility** | wrappers with the same interface; middleware is the chain where any link may stop |
| control access to the same interface (cache, lazy, permissions, remote) | **Proxy** | a stand-in the caller cannot distinguish from the subject; if it adds methods it is a decorator, not a proxy |
| build a complex object with optional parts | **Builder** | pays past ~4 constructor arguments or when step order matters; C++20 designated initialisers may be enough |
| choose the concrete type at runtime without exposing the constructor | **Factory Method** | a function returning the interface; **Abstract Factory** only when the variants must be consistent as a family |
| react to change without knowing who listens | **Observer** / pub-sub | an event emitter; decide delivery order, sync vs async, and who removes a listener |
| fit a foreign interface to yours | **Adapter** | a translating wrapper; **Facade** when the goal is a smaller entry point to a subsystem, not a translation |
| behaviour depends on lifecycle state | **State** | Strategy's shape, but the transitions are the point — write them as a table |
| work must be queued, undone or logged | **Command** | a value carrying its receiver and arguments; undo needs a history, not just the interface |
| traverse without exposing internals | **Iterator** | usually already there (`begin`/`end`, generators, async iteration) |
| one operation over a closed set of types | **Visitor** | in C++ `std::variant` + `std::visit`, or one `switch`, beats double dispatch; Visitor pays when the type set is open |
| one shared instance | **not Singleton** | pass it in; the lifetime belongs to the composition root, not to the class |

Two rows have aged worst. Singleton is the one the catalogue's own author wanted
removed, and it is usually an anti-pattern now: it hides a dependency behind a global
accessor and makes substitution in a test impossible — the fix is a parameter or a
module-level value, not a pattern. Inheritance-heavy factories, a class per family, are
the other: a factory function returns the same decoupling with nothing to subclass, and
the enum of variants rarely needs a hierarchy of its own.

Judge a row by whether a second variant exists (that is the *when to reach* entry); the
name buys vocabulary for a change you have, not a licence for one you might.

## Run it

```run
python3 - <<'PY'
# The rows differ by what the caller may rely on, not by the wrapping technique.
class Subject:
    def count(self): return 3

def surface(obj):
    return {name for name in dir(obj) if not name.startswith("_")}

class Proxy:                        # same surface: substitutable for the subject
    def __init__(self, inner): self._inner = inner
    def count(self): return self._inner.count()

class Decorator:                    # superset: the caller must know the extra
    def __init__(self, inner): self._inner = inner
    def count(self): return self._inner.count()
    def cached_count(self): return self.count()

class Adapter:                      # renamed: a foreign vocabulary, translated
    def __init__(self, foreign): self._foreign = foreign
    def count(self): return self._foreign.size()

assert surface(Proxy(Subject())) == surface(Subject())
assert surface(Decorator(Subject())) > surface(Subject())
foreign = type("Foreign", (), {"size": lambda self: 3})()
assert Adapter(foreign).count() == 3
print("proxy invisible, decorator visible, adapter translating")
PY
```
