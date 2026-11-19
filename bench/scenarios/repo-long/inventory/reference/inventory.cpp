#include "inventory.h"

int total_units(const std::vector<Item>& items) { int total = 0; for (const auto& item : items) total += item.units; return total; }

int total_cents(const std::vector<Item>& items) {
    int total = 0;
    for (const auto& item : items) total += item.units * item.price_cents;
    return total;
}
