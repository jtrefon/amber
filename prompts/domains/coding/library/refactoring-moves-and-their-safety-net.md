trigger: when the behaviour must not change but the shape must — work in named moves, each one checked before the next
tags: #design #testing
also: refactor, refactoring
weight: 55
verified: 2026-10
reference: https://refactoring.com/catalog/
sources: Fowler, Refactoring 2ed (ch. 2 Two Hats and self-testing code, ch. 4 Building Tests, ch. 6 A First Set of Refactorings, ch. 8 Move Function); refactoring.com catalogue pages for the named moves; martinfowler.com/bliki/SelfTestingCode.html; claims executed by their run block

Refactoring is a chain of named moves, each small enough to be obviously
behaviour-preserving. The catalogue name is the unit of thought: say Extract
Function, not "clean this up", and read the entry before improvising mechanics.

The net comes first. Every move is judged by a test run, so a suite that one
command can run is the precondition for the work, not a reward for finishing it;
on code without one, the first move is the test that captures today's behaviour.
Green keeps the move, red undoes it — a red between two moves means the move was
too big, not that the code needs debugging.

The small moves, and what makes each one safe:

- **Extract Function** / **Inline Function** are inverses. Extract when a reader
  must reconstruct what a fragment does: name the function for the intent, pass
  the locals it needs. If it captures too many, back off and do the smaller move
  first (Extract Variable, Replace Temp with Query). Inline when the name says
  no more than the body, or when delegation hides the flow.
- **Rename Variable** and **Change Function Declaration.** A name is the
  cheapest documentation; a compiler-checked rename (IDE or LSP) updates every
  reference, a search-and-replace only guesses. On a published declaration, keep
  the old entry point delegating to the new one and migrate callers one at a
  time, so no caller is broken between runs.
- **Move Function** / **Move Field.** Send a function to the context it
  references most — usually the data it works on — judging by its callers and
  the calls it makes. When several functions share one body of data, **Combine
  Functions into Class** gives them a home in one move.
- **Extract Variable** names an expression before the code around it changes;
  **Inline Variable** undoes it when the name stops earning its line.
- **Introduce Parameter Object.** Data that travels together call after call
  becomes one value; signatures shorten and the concept has somewhere to grow.
- **Split Phase.** One function doing two jobs over different data — parse then
  price, decode then dispatch — becomes two sequential functions, the first
  producing an intermediate value the second consumes.
- **Replace Inline Code with Function Call** finishes an extraction: the copy of
  the fragment elsewhere now calls the new name.

Two moves between test runs are one move nobody can review, and a move that
needs a debugger is a behaviour change wearing a refactoring's name. What makes
each move on the list safe is the same: mechanical, small, reverted when red.

## Run it

```run
# The discipline in one loop: the net runs before and after the move, and a red
# step is undone instead of debugged. Split Phase here — parse out of pricing.
# -B is deliberate: a stale __pycache__ would happily pass a test the source fails.
git -c init.defaultBranch=main init -q .
cat > price.py <<'PY'
PRICES = {"apple": 2, "pear": 3}

def total(line):
    qty, item = line.split()
    return int(qty) * PRICES[item]
PY
cat > test_price.py <<'PY'
from price import total

assert total("3 apple") == 6
assert total("1 pear") == 3
PY
git add -A && git -c user.email=net@example -c user.name=net commit -qm "capture the behaviour"
python3 -B test_price.py && echo "green before the move"

cat > price.py <<'PY'
PRICES = {"apple": 2, "pear": 3}

def parse(line):
    qty, item = line.split()
    return int(qty), item

def total(line):
    qty, item = parse(line)
    return qty * PRICES[item]
PY
python3 -B test_price.py && echo "green after Split Phase"
git add -A && git -c user.email=net@example -c user.name=net commit -qm "split parse from pricing"

cat > price.py <<'PY'
PRICES = {"apple": 2, "pear": 3}

def parse(line):
    qty, item = line.split()
    return item, int(qty)

def total(line):
    qty, item = parse(line)
    return qty * PRICES[item]
PY
if python3 -B test_price.py 2>/dev/null; then echo "a wrong move stayed green"; exit 1; fi
git checkout -q -- price.py
python3 -B test_price.py && echo "red reverted, green kept"
```
