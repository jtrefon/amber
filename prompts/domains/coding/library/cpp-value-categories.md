trigger: when a value is copied where a move was meant, or std::move made things worse
tags: #cpp #memory #ownership
weight: 65
verified: 2026-10
reference: https://en.cppreference.com/w/cpp/language/value_category

Three questions decide what the compiler calls, and the answers are not what the
syntax suggests:

- **Is the expression an lvalue, an xvalue or a prvalue?** A name is always an
  lvalue — `x` never moves, however obvious "we are done with it" looks.
- **Does the target have a move constructor?** A type without one falls back to
  copying, silently.
- **Is the source const?** `std::move` on a `const` object yields a `const T&&`,
  which no move constructor can bind to, so it copies. This is the classic
  "I added std::move and it got slower".

Returning a local by value is already a move (or elided entirely) — a `return
std::move(x);` *pessimises* it by disabling copy elision. Moving from a
moved-from object is legal but its value is unspecified: reset it if anything
might read it.

## Run it

```run
cat > move_demo.cpp <<'SRC'
#include <cstdio>
#include <utility>

struct Probe {
    Probe() = default;
    Probe(const Probe&) { std::puts("copy"); }
    Probe(Probe&&) noexcept { std::puts("move"); }
};

int main() {
    Probe probe;
    const Probe frozen;
    std::puts("-- lvalue");       Probe a(probe);
    std::puts("-- std::move");    Probe b(std::move(probe));
    std::puts("-- move a const"); Probe c(std::move(frozen));
    (void)a; (void)b; (void)c;
}
SRC
g++ -std=c++17 -O0 move_demo.cpp -o move_demo
./move_demo | sed -n '/move a const/{n;p}' | grep -q copy
```
