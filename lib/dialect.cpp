
#include "agent/dialect.h"
#include "agent/dialect_openai.h"

#include <map>

namespace agent {

namespace {

using DialectFactory = std::function<std::unique_ptr<Dialect>()>;

std::map<std::string, DialectFactory>& dialect_table() {
    static std::map<std::string, DialectFactory> table = {
        {"openai", []() { return make_openai_dialect(); }},
    };
    return table;
}

} // namespace

std::unique_ptr<Dialect> make_dialect(const std::string& flavor) {
    auto& table = dialect_table();
    auto it = table.find(flavor);
    if (it == table.end()) it = table.find("openai");
    return it->second();
}

void register_dialect(const std::string& flavor,
                      std::function<std::unique_ptr<Dialect>()> factory) {
    dialect_table()[flavor] = std::move(factory);
}

} // namespace agent
