#include "pricing.h"

class ShippingStrategy {
public:
    virtual ~ShippingStrategy() = default;
    virtual double cost(double weight_kg) const = 0;
};

class StandardShipping : public ShippingStrategy {
public:
    double cost(double weight_kg) const override { return 10 + 2 * weight_kg; }
};

class ExpressShipping : public ShippingStrategy {
public:
    double cost(double weight_kg) const override { return 20 + 4 * weight_kg; }
};

static double apply(const ShippingStrategy& s, double weight_kg) {
    return s.cost(weight_kg);
}

double shipping_cost(const std::string& mode, double weight_kg) {
    if (mode == "standard") return apply(StandardShipping(), weight_kg);
    if (mode == "express") return apply(ExpressShipping(), weight_kg);
    return -1;
}
