// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include <agent.h>
#include <cassert>
#include <iostream>

int main() {
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::register_default_tools(reg, jobs);

    // read tool
    {
        agent::Tool* rd = reg.find("read");
        assert(rd);
        auto r = rd->execute({{"path", "prompts/system.md"}, {"limit", 3}});
        assert(r.ok);
        std::cout << "[read] ok=" << r.ok << " has-content="
                  << (!r.output.empty()) << "\n";
    }

    // write tool (create temp file)
    {
        agent::Tool* wr = reg.find("write");
        assert(wr);
        auto r = wr->execute({
            {"path", "/tmp/amber-smoke.txt"},
            {"edits", {{{"old", ""}, {"new", "line one\nline two\n"}}}}
        });
        assert(r.ok);
        std::cout << "[write] " << r.output << "\n";

        // patch it
        auto r2 = wr->execute({
            {"path", "/tmp/amber-smoke.txt"},
            {"edits", {{{"old", "line two"}, {"new", "line 2"}}}}
        });
        assert(r2.ok);
        std::cout << "[write-patch] " << r2.output << "\n";
    }

    // search tool
    {
        agent::Tool* se = reg.find("search");
        assert(se);
        auto r = se->execute({{"pattern", "register_default_tools"},
                              {"path", "."}, {"glob", "*.cpp"}});
        std::cout << "[search:grep] ok=" << r.ok << " output:\n"
                  << r.output << "\n";
    }

    // semantic mode (lexical index + cosine ranking)
    {
        agent::Tool* se = reg.find("search");
        assert(se);
        auto r = se->execute({{"pattern", "register the default tools"},
                              {"path", "."}, {"glob", "*.cpp"},
                              {"mode", "semantic"}});
        std::cout << "[search:semantic] ok=" << r.ok << " output:\n"
                  << r.output << "\n";
        assert(r.ok);
    }

    std::cout << "smoke test passed\n";
    return 0;
}
