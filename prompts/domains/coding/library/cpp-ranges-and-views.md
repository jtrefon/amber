trigger: when a loop is transforming or filtering a container and a view would say it better
tags: #cpp
weight: 45
verified: 2026-10
reference: https://en.cppreference.com/w/cpp/ranges

A view is a lazy description of a sequence; the algorithm runs when something
consumes it. That is the whole idea, and it is also where the traps live.

- **Compose, do not nest**: `values | views::filter(p) | views::transform(f)`
  reads in the order it executes.
- **A view of a temporary dangles.** `auto v = make_vector() | views::take(3);`
  holds iterators into a destroyed vector. Assign to a container when the range
  must outlive the expression.
- **The ranges algorithms take projections**: `std::ranges::sort(people, {}, &Person::age)`
  sorts by a member without a lambda.
- **Laziness is not free**: a chain of filters over a small range is slower than a
  loop, and a pipeline evaluated twice is evaluated twice. Measure when it matters.
- **A plain loop is still the right answer** when the body has early exits or
  side effects: a pipeline that needs `break` should not be a pipeline.

## Run it

```run
cat > ranges_demo.cpp <<'SRC'
#include <cstdio>
#include <ranges>
#include <vector>

int main() {
    std::vector<int> values{1, 2, 3, 4, 5, 6};
    auto evens = values | std::views::filter([](int v) { return v % 2 == 0; }) |
                 std::views::transform([](int v) { return v * 10; });
    int sum = 0;
    for (int v : evens)
        sum += v;
    std::printf("%d\n", sum);
}
SRC
g++ -std=c++20 ranges_demo.cpp -o ranges_demo
./ranges_demo | grep -q 120
```
