trigger: when code works in debug and fails in release, or a bug makes no sense at all
tags: #cpp #memory #debugging
weight: 60
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://en.cppreference.com/w/cpp/language/ub

Undefined behaviour is a contract breach, not a crash: the compiler is allowed to
assume it never happens, so the symptom is usually far from the cause.

The catalogue worth recognising by symptom:

- **Signed overflow, shift past width, division by zero** — arithmetic that
  silently "works" in `-O0` and miscompiles in `-O2`. Use unsigned for bit
  manipulation, and check the shift count.
- **Out-of-bounds and iterator invalidation** — the write lands somewhere; ASan
  names both the access and the object.
- **Use after free and double free** — ASan, with the allocation and free sites.
- **Uninitialised reads** — MSan (clang) or valgrind, not ASan; the value is
  whatever the stack held.
- **Data races** — TSan; a race is UB even without a visible inconsistency.
- **Lifetime / strict-aliasing violations** — a cast that reinterprets a type
  the compiler may assume unchanged; `std::bit_cast` and `memcpy` are the
  defined versions.
- **Violating `noexcept`, a null `this`, a `delete` of a non-virtual base**
  — each terminates in a way that looks like a crash in unrelated code.

Reach for `-fsanitize=address,undefined` before reaching for the debugger: the
report names the fault, the debugger only shows you where it hurt.

## Run it

```run
cat > ub_demo.cpp <<'SRC'
#include <cstdio>

int sum_positive(int limit) {
    int total = 0;
    for (int i = 1; i <= limit; ++i)
        total += i;
    return total;
}

int main() {
    // Signed overflow is undefined: the optimiser may assume the loop terminates.
    std::printf("%d\n", sum_positive(1000000));
}
SRC
g++ -std=c++17 -fsanitize=undefined -g -O1 ub_demo.cpp -o ub_demo
grep -q "signed integer overflow" <(./ub_demo 2>&1 || true)
```
