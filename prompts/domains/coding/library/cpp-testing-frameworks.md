trigger: when picking or wiring the C++ test framework for a project
tags: #cpp #testing
weight: 45
verified: 2026-10
reference: https://google.github.io/googletest/

The framework matters less than three properties; pick whichever gives them with
the least friction where the project already builds.

- **GoogleTest** — the default for projects already using CMake and Google-style
  names; the largest ecosystem, and gmock included. Heaviest of the three.
- **Catch2 v3** — one header (or one library), expressive BDD-style sections,
  good defaults; a good fit for a library with small, self-contained tests.
- **doctest** — the fastest to compile, single header, the least magic; the right
  choice when the test build is the slow part of the loop.
- **Google Benchmark** for measurement (a different job from assertion), and
  **libFuzzer** for input nobody thought of — a test suite runs a fixed corpus,
  a fuzzer reaches the inputs you did not write.

Whatever the framework: one assertion per behaviour, the failure message
carries the values, and a test that cannot fail is worse than no test. In CI,
also see the sanitizer builds — a green suite under ASan is a different claim
from a green suite.

## Run it

```run
# manual: needs the project's dependency manager; the shape is target_link_libraries(tests PRIVATE GTest::gtest_main)
echo "pick the framework the project already builds with"
```
