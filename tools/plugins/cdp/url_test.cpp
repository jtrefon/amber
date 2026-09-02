// Hermetic unit test for cdp::Url — no server, no network. The two wss://
// rows are the regression guard for the find('/', 7) bug: they fail on the
// old offset-based code and pass on the table-driven scheme translation.
#include "url.h"

#include <iostream>
#include <string>

static int failed = 0;
#define ASSERT(cond) do { \
    if (!(cond)) { std::cerr << "FAIL: " << #cond << "\n"; failed++; } \
} while (0)
#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { std::cerr << "FAIL: " << #a << " == " << #b \
        << "\n  got:      " << (a) << "\n  expected: " << (b) << "\n"; failed++; } \
} while (0)

namespace {

void check(const std::string& in, const std::string& want_scheme,
           const std::string& want_base) {
    cdp::Url u;
    bool ok = cdp::Url::parse(in, u);
    ASSERT(ok);
    ASSERT_EQ(u.scheme, want_scheme);
    ASSERT_EQ(u.toHttpBase(), want_base);
}

}  // namespace

int main() {
    // ws:// — already-correct path (must not regress).
    check("ws://localhost:9222/devtools/page/ABC", "ws", "http://localhost:9222");
    check("ws://127.0.0.1:9222", "ws", "http://127.0.0.1:9222");
    check("ws://host", "ws", "http://host");

    // wss:// — the regression guard (was "https:/" before the fix).
    check("wss://localhost:9222/devtools/page/ABC", "wss", "https://localhost:9222");
    check("wss://127.0.0.1:9222", "wss", "https://127.0.0.1:9222");
    check("wss://host", "wss", "https://host");

    // http/https — already-HTTP endpoints pass through.
    check("http://host:8080/x", "http", "http://host:8080");
    check("https://host:8443/x", "https", "https://host:8443");

    // malformed — parse must reject.
    cdp::Url u;
    ASSERT(!cdp::Url::parse("not-a-url", u));
    ASSERT(!cdp::Url::parse("ws://", u));

    if (failed) {
        std::cerr << failed << " URL test(s) FAILED\n";
        return 1;
    }
    std::cout << "url_test: all passed\n";
    return 0;
}
