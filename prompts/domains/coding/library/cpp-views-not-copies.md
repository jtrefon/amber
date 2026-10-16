trigger: when a function takes a string or a buffer and the call site copies more than it should
tags: #cpp #memory #perf
weight: 55
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://en.cppreference.com/w/cpp/string/basic_string_view

A view is a pointer and a length: cheap to pass, and dangerous for exactly the
same reason.

- **Take `std::string_view` for input**, `std::string` (or `std::string&&`) when
  you must own or store it, and `const std::string&` almost never — it forces the
  caller to own a string it may not have.
- **A view must not outlive its owner.** A view of a temporary, of a local, or of
  a member of a moved-from object is undefined behaviour the compiler will not
  catch. Returning a `string_view` to something local is the canonical mistake.
- **A view is not null-terminated.** Anything that takes a `const char*` needs
  `std::string(view)` or `%.*s`; this is where a view silently reads past its end.
- **`std::span<T>` is the same idea for arrays and buffers** — pass a span instead
  of pointer-plus-length, and the callee cannot get the two out of step.
- **Watch the conversions**: `const char*` to `string_view` is cheap; `string_view`
  to `std::string` allocates, and a function that returns `std::string` from a view
  allocates at every call site.
- In a header-only or template interface, take what the caller has rather than what
  you find convenient: `std::string_view` in, owned only where ownership is real.

## Run it

```run
cat > view_trap.cpp <<'SRC'
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

std::string_view dangling() {
    auto owned = std::make_unique<std::string>("storage freed before the view is read");
    return std::string_view(*owned);
}

int main() {
    std::string_view view = dangling();
    std::printf("%c\n", view[0]);
}
SRC
g++ -std=c++17 -fsanitize=address -g -O1 view_trap.cpp -o view_trap
grep -q "heap-use-after-free" <(./view_trap 2>&1 || true)
```
