trigger: when a unit test needs a database, a clock or a network, or a test passes with the double and fails in production
tags: #cpp #testing
also: mock, stub, fake
weight: 45
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://google.github.io/googletest/gmock_cook_book.html

A double is a claim about how the real thing behaves; every wrong claim is a bug
the test suite will defend.

- **A seam is what makes it possible**: an interface (virtual), a template
  parameter, or a function passed in. If the dependency is constructed inside the
  unit, no double can help — that is the refactor, not the test.
- **Prefer the real thing when it is cheap.** An in-memory implementation of the
  interface (a map instead of a database) keeps the behaviour honest and needs no
  framework.
- **A fake over a mock**: a fake implements the contract (in-memory repo), a mock
  asserts the conversation (`EXPECT_CALL`). Mocks couple the test to the *order*
  and *shape* of calls, which is why a refactor breaks a hundred tests that were
  asserting implementation rather than behaviour.
- **Never double the thing under test**, and never let a double be the only
  implementation — a fake that nobody checks against the real one drifts, and the
  drift is discovered in production.
- **Determinism is the reason to double**: an injected clock, a seeded random, an
  in-memory transport. A test that sleeps is a test that fails on a loaded CI box.
- **Contract tests close the loop**: one test per double that runs the real
  implementation through the same interface, so the fake's claim is checked.

## Run it

```run
# manual: needs the project's test framework; the shape is an injected interface,
# a fake for behaviour, and one contract test against the real implementation
echo "double the boundary, never the subject"
```
