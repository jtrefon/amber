#include "url.h"

#include <optional>
#include <string_view>
#include <utility>

namespace cdp {
namespace {

// Scheme translation as data: each row is an isolated mapping. Adding a
// scheme is a row, not a branch; ws and wss never reference each other.
const std::pair<std::string_view, std::string_view> kHttpScheme[] = {
    {"ws", "http"},
    {"wss", "https"},
    {"http", "http"},
    {"https", "https"},
};

std::optional<std::string> http_scheme(const std::string& s) {
    for (const auto& [from, to] : kHttpScheme)
        if (s == from) return std::string(to);
    return std::nullopt;
}

}  // namespace

bool Url::parse(const std::string& in, Url& out) {
    size_t sep = in.find("://");
    if (sep == std::string::npos || sep == 0) return false;
    out.scheme = in.substr(0, sep);
    std::string rest = in.substr(sep + 3);  // host[:port][/path]

    size_t slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    out.path = (slash == std::string::npos) ? "" : rest.substr(slash);

    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        out.host = hostport.substr(0, colon);
        out.port = hostport.substr(colon + 1);
    } else {
        out.host = hostport;
        out.port.clear();
    }
    return !out.host.empty();
}

std::string Url::authority() const {
    return port.empty() ? host : host + ":" + port;
}

std::string Url::toHttpBase() const {
    auto s = http_scheme(scheme);
    if (!s) return "";
    return *s + "://" + authority();
}

}  // namespace cdp
