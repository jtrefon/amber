
#include <agent.h>
#include <cassert>
#include <iostream>

namespace {

void smoke_read(agent::ToolRegistry& reg) {
    auto rd = reg.find("read");
    assert(rd);
    auto r = rd->execute({{"path", "prompts/system.md"}, {"limit", 3}});
    assert(r.ok);
    std::cout << "[read] ok=" << r.ok << " has-content=" << (!r.output.empty()) << "\n";
}

void smoke_write(agent::ToolRegistry& reg) {
    auto wr = reg.find("write");
    assert(wr);
    auto r = wr->execute(
        {{"path", "amber-smoke.txt"}, {"edits", {{{"old", ""}, {"new", "line one\nline two\n"}}}}});
    assert(r.ok);
    std::cout << "[write] " << r.output << "\n";

    // patch it
    auto r2 = wr->execute(
        {{"path", "amber-smoke.txt"}, {"edits", {{{"old", "line two"}, {"new", "line 2"}}}}});
    assert(r2.ok);
    std::cout << "[write-patch] " << r2.output << "\n";
    std::remove("amber-smoke.txt");
}

void smoke_search_grep(agent::ToolRegistry& reg) {
    auto se = reg.find("search");
    assert(se);
    auto r = se->execute({{"pattern", "register_default_tools"}, {"path", "."}, {"glob", "*.cpp"}});
    std::cout << "[search:grep] ok=" << r.ok << " output:\n" << r.output << "\n";
}

// semantic mode (lexical index + cosine ranking)
void smoke_search_semantic(agent::ToolRegistry& reg) {
    auto se = reg.find("search");
    assert(se);
    auto r = se->execute({{"pattern", "register the default tools"},
                          {"path", "."},
                          {"glob", "*.cpp"},
                          {"mode", "semantic"}});
    std::cout << "[search:semantic] ok=" << r.ok << " output:\n" << r.output << "\n";
    assert(r.ok);
}

} // namespace

int main() {
    agent::ToolRegistry reg;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::register_default_tools(reg, jobs, todos);

    smoke_read(reg);
    smoke_write(reg);
    smoke_search_grep(reg);
    smoke_search_semantic(reg);

    std::cout << "smoke test passed\n";
    return 0;
}
