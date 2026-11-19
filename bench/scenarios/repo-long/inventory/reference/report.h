#pragma once
#include <string>
#include <vector>
#include "inventory.h"
std::string format_line(const Item& item);
std::string format_report(const std::vector<Item>& items);
