trigger: when a link error or an ODR surprise makes no sense from the code you can see
tags: #cpp #tooling #build
weight: 60
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://en.cppreference.com/w/cpp/language/odr

The compiler sees one translation unit; the linker sees symbols. Most "impossible"
C++ errors live between the two.

- **Inline is a linkage promise, not a speed hint.** A function defined in a header
  must be `inline`, a template, or inside a class — otherwise every TU emits it and
  the linker picks one, silently discarding the others.
- **Overloads are mangled names.** `nm -C` (demangled) shows what actually got
  emitted; a symbol you expected and cannot find is the answer to most link errors.
- **ODR violations are undefined behaviour, not diagnostics.** Two different
  definitions of the same inline function across TUs — a changed header without a
  rebuild, an `#ifdef` that differs per TU — link happily and behave impossibly.
  That is why a struct layout change without a clean rebuild produces memory
  corruption rather than an error.
- **Static initialisation order** across TUs is unspecified; a global that needs
  another global is a bug waiting for a different link order.

When a link error resists reading: look at the symbol, not the code.

## Run it

```run
cat > odr_probe.cpp <<'SRC'
int add(int a, int b) { return a + b; }
int main() { return add(2, 2) == 4 ? 0 : 1; }
SRC
g++ -std=c++17 -c odr_probe.cpp -o odr_probe.o
nm -C odr_probe.o | grep -q "T add(int, int)"
```
