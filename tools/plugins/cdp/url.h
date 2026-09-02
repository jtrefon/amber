// URL value type for the CDP plugin. Pure, dependency-free, no globals:
// parse() splits a ws:// / wss:// / http:// / https:// endpoint into fields,
// toHttpBase() derives the HTTP(S) base for the /json endpoint. The scheme
// translation is a data table (ws->http, wss->https), not control flow, so
// adding a scheme is a row, not a branch, and each scheme is isolated.
#ifndef CDP_URL_H
#define CDP_URL_H

#include <string>

namespace cdp {

struct Url {
    std::string scheme;  // "ws" | "wss" | "http" | "https"
    std::string host;
    std::string port;    // empty if none
    std::string path;    // leading '/' included, empty if none

    // Splits "scheme://host[:port][/path]" into fields. Returns false if the
    // input has no "scheme://" prefix or an empty host.
    static bool parse(const std::string& in, Url& out);

    std::string authority() const;  // "host" or "host:port"
    std::string toHttpBase() const;  // "http(s)://host[:port]" (no path)
};

}  // namespace cdp

#endif  // CDP_URL_H
