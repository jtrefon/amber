# Task: polymorphism over switch

`shapes.cpp` models shapes with a `type` enum and computes area with a switch
statement. Refactor it to a polymorphic design:

- an abstract base class `Shape` with a pure virtual `double area() const`
- derived classes `Circle` and `Square`
- the public entry points `double area_circle(double r)` and
  `double area_square(double s)` must keep their exact behavior

Behavior must not change: hidden tests compare the exact area output before
and after. Do not change `shapes.h`.
