#include "report.h"

std::string format_line(const Item& item) { return item.name + ": " + std::to_string(item.units) + " @ " + std::to_string(item.price_cents) + "c"; }

std::string format_report(const std::vector<Item>& items) {
    std::string out;
    for (const auto& item : items) out += format_line(item) + "\n";
    return out;
}
