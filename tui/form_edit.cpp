#include "widgets.h"

#include "tui/modal_host.h"

#include <string>
#include <utility>
#include <vector>

namespace tui {

bool form_edit(const std::string& title, std::vector<FieldSpec>& fields,
               const std::string& notice) {
    return ModalHost{}.form(title, fields, notice);
}

} // namespace tui
