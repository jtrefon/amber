trigger: when a library ships to someone else, or a variant would beat an inheritance tree
tags: #cpp #abi
weight: 45
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://en.cppreference.com/w/cpp/language/virtual

The ABI is what the caller and the built artefact agree on: name mangling, struct
layout, calling convention, exception tables, and the exact implementation of
anything in the standard library that crosses the boundary.

- **Keep the header stable, the internals free**: the pimpl idiom (or an opaque
  handle) means a change inside does not force a rebuild of every consumer.
- **`std::string` and `std::vector` in an interface** are fine when everyone
  builds with the same standard library and flags, and a trap when they do not —
  `_GLIBCXX_USE_CXX11_ABI` mismatches are the classic version of this.
- **Inline namespaces version the ABI** (`namespace v1 {}` inside `inline
  namespace v1 {}`), which is how a library ships two layouts at once.
- **Virtual for an open set, `std::variant` for a closed one.** A variant of
  alternatives is a `switch` (or `std::visit`) with exhaustiveness the compiler
  checks; a base class with two implementations is a vtable and a heap
  allocation nobody needed.
- **`std::function` erases the type and copies the callable** — fine at a
  boundary, wasteful in a hot loop; a template parameter costs nothing but must
  be visible in a header.
- **Adding a virtual function to a class others derive from** breaks their
  vtable; adding a pure one breaks their build loudly, which is the better of
  the two.

## Run it

```run
cat > abi_demo.cpp <<'SRC'
#include <cstdio>
#include <variant>

struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
};

struct Square : Shape {
    double side = 2;
    double area() const override { return side * side; }
};

int main() {
    Square square;
    Shape& open = square;
    std::variant<int, Square> closed = square;
    std::printf("%.0f %.0f\n", open.area(), std::get<Square>(closed).area());
}
SRC
g++ -std=c++17 abi_demo.cpp -o abi_demo
./abi_demo | grep -q "4 4"
```
