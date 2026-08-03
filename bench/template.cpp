
// RED: stub — template engine not implemented yet (always fails).

#include "bench/template.h"

namespace bench {

bool load_structure_checks(const std::string&, std::vector<StructureCheck>&,
                           std::string& err) {
    err = "not implemented";
    return false;
}

TemplateResult run_template(const std::string&, const std::string&,
                            const std::string&, std::string& err) {
    err = "not implemented";
    return TemplateResult{};
}

} // namespace bench
