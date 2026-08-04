#include "pricing.h"

double shipping_cost(const std::string& mode, double weight_kg) {
    if (mode == "standard") return 10 + 2 * weight_kg;
    if (mode == "express") return 20 + 4 * weight_kg;
    return -1;
}
