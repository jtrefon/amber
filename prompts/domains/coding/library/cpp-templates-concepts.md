trigger: when a template failure is a wall of errors, or a constraint is being written for the first time
tags: #cpp #templates
also: concepts, templates, generics
weight: 60
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://en.cppreference.com/w/cpp/language/constraints

A concept names the requirement where the error appears, which is the whole
point: the alternative is a diagnostic thirty instantiations deep.

- **Write the concept, not the SFINAE.** `std::enable_if` still appears in old
  code; new code says `requires`, and the failure names the unsatisfied
  requirement.
- **`requires` expressions check syntax; `static_assert` checks meaning.** A type
  can be `Addable` and still not mean what the algorithm needs — say that in the
  body.
- **The standard concepts cover most cases**: `std::derived_from`,
  `std::invocable`, `std::ranges::range`, `std::totally_ordered`. Reach for those
  before writing one.
- **Constrain `auto` too** (`Addable auto value`), which is how a generic lambda
  becomes readable.
- **A plain overload beats a constrained template** when the set of types is
  known: overload resolution has been solving that problem for forty years.

## Run it

```run
cat > concepts_demo.cpp <<'SRC'
#include <concepts>
#include <cstdio>

template <typename T>
concept Addable = requires(T a, T b) {
    { a + b } -> std::same_as<T>;
};

template <Addable T>
T twice(T value) {
    return value + value;
}

int main() { std::printf("%d\n", twice(21)); }
SRC
g++ -std=c++20 concepts_demo.cpp -o concepts_demo
./concepts_demo | grep -q 42
```
