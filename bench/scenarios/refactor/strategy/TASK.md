# Task: strategy pattern

`pricing.cpp` computes shipping cost with if/else on a mode string. Refactor
into a strategy design:

- an abstract `ShippingStrategy` with `virtual double cost(double weight_kg) const`
- implementations `StandardShipping` (10 + 2*w) and `ExpressShipping` (20 + 4*w)
- keep the public entry `double shipping_cost(const std::string& mode, double weight_kg)`
  with identical behavior (unknown mode -> -1)

Behavior must not change: hidden tests compare exact outputs. Do not change
`pricing.h`.
