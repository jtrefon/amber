// MCP stdio test server that ignores SIGTERM and writes its PID to a file
// (C++ replacement for mcp_ignore_sigterm.py).
//
// Usage: mcp_ignore_sigterm <pidfile>
//
// The transport must escalate from SIGTERM to SIGKILL to terminate it; the
// pidfile lets the test read the child's PID after the fact.
#include <csignal>
#include <cstdio>
#include <unistd.h>

int main(int argc, char** argv) {
    std::signal(SIGTERM, SIG_IGN);
    if (argc > 1) {
        FILE* f = std::fopen(argv[1], "w");
        if (f != nullptr) {
            std::fprintf(f, "%d", static_cast<int>(getpid()));
            std::fclose(f);
        }
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
        if (n <= 0) sleep(3600);  // EOF: keep running, awaiting SIGKILL
    }
}