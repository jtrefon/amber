#pragma once
#include <string>
#include <vector>
struct Item { std::string name; int units; int price_cents; };
int total_units(const std::vector<Item>& items);
int total_cents(const std::vector<Item>& items);
