trigger: when deciding how a function reports failure
tags: #cpp #errors
weight: 55
verified: 2026-10
reference: https://en.cppreference.com/w/cpp/utility/expected

Pick the mechanism per layer and say which one in the interface, because mixing
them is how "impossible" states appear.

- **Exceptions are for the caller cannot continue** — a corrupt invariant, a
  failed allocation, a constructor that cannot establish its postcondition. This
  is why a constructor that can fail is either throwing or a factory function,
  not an `init()` nobody remembers to call.
- **`std::expected<T, E>` (or `outcome`) for expected failures** — parse errors,
  validation, "not found" — where the caller must handle each case anyway.
  `std::optional` when there is only one failure meaning.
- **`noexcept` is a promise with teeth**: throwing through it terminates the
  process. Mark moves and destructors `noexcept`; get the semantics right rather
  than the annotation.
- **Do not throw across a boundary you do not control** — a C API, a callback
  from a C library, a `main` that must report an exit code. Catch at the boundary
  and translate.
- **`-fno-exceptions` is a real mode** (embedded, some game platforms), and it
  makes `std::optional`/`std::expected` error handling mandatory, not stylistic.
- **Unwinding costs nothing when it does not happen** and a lot when it does;
  exceptions used for control flow in a hot loop is the pattern to refactor.

## Run it

```run
cat > no_exceptions.cpp <<'SRC'
#include <cstdio>
#include <optional>

std::optional<int> parse_positive(int value) {
    if (value <= 0)
        return std::nullopt;
    return value;
}

int main() {
    std::printf("%d %d\n", parse_positive(3).value_or(-1), parse_positive(-3).value_or(-1));
}
SRC
g++ -std=c++17 -fno-exceptions -Wall -Wextra no_exceptions.cpp -o no_exceptions
./no_exceptions | grep -q "3 -1"
```
