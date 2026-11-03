// The idle hint rotator: delay, rotation, one cycle per idle period and the
// static data file. No clock, no terminal.

#include "tui/hints.h"
#include "test_util.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using tui::HintRotator;

TEST(hints_wait_for_the_idle_delay) {
    HintRotator rotator({"a", "b"}, 7);
    ASSERT_EQ(rotator.update(4999ms, true), std::string{});
    ASSERT_EQ(rotator.update(5000ms, true).empty(), false);
}

TEST(hints_rotate_on_the_interval) {
    HintRotator rotator({"a", "b", "c"}, 7);
    const std::string first = rotator.update(5000ms, true);
    ASSERT_EQ(rotator.update(16999ms, true), first);
    const std::string second = rotator.update(17000ms, true);
    ASSERT(second != first);
}

TEST(hints_are_hidden_and_restart_when_ineligible) {
    HintRotator rotator({"a", "b", "c"}, 7);
    ASSERT_FALSE(rotator.update(5000ms, true).empty());
    ASSERT_EQ(rotator.update(6000ms, false), std::string{});
    ASSERT_EQ(rotator.update(0ms, true), std::string{});
    ASSERT_FALSE(rotator.update(5000ms, true).empty());
}

TEST(hints_show_one_cycle_then_stay_quiet) {
    HintRotator rotator({"a", "b"}, 7);
    ASSERT_FALSE(rotator.update(5000ms, true).empty());
    ASSERT_FALSE(rotator.update(17000ms, true).empty());
    ASSERT_EQ(rotator.update(29000ms, true), std::string{});
}

TEST(hints_do_not_repeat_inside_one_bag) {
    HintRotator rotator({"a", "b", "c"}, 7);
    std::set<std::string> seen;
    for (auto idle : {5000ms, 17000ms, 29000ms})
        seen.insert(rotator.update(idle, true));
    ASSERT_EQ(seen.size(), 3u);
}

TEST(hints_reset_hides_the_current_text) {
    HintRotator rotator({"a", "b"}, 7);
    ASSERT_FALSE(rotator.update(5000ms, true).empty());
    rotator.reset();
    ASSERT_EQ(rotator.update(5000ms, true).empty(), false); // restart is eligible
}

TEST(hints_empty_and_filtered_lists_stay_quiet) {
    HintRotator empty;
    ASSERT(empty.empty());
    ASSERT_EQ(empty.update(10000ms, true), std::string{});

    HintRotator filtering({"", "real", ""}, 7);
    ASSERT_FALSE(filtering.empty());
    ASSERT_EQ(filtering.update(5000ms, true), std::string("real"));
    filtering.set_hints({});
    ASSERT(filtering.empty());
    ASSERT_EQ(filtering.update(20000ms, true), std::string{});
}

TEST(hints_load_rejects_malformed_and_wrong_shapes) {
    const std::string path = "/tmp/amber-hints-" + std::to_string(::getpid()) + ".json";
    auto write = [&](const char* text) {
        std::ofstream out(path);
        out << text;
    };
    write("{");
    ASSERT_EQ(tui::load_hints(path).size(), 0u);
    write(R"({"hints":"no"})");
    ASSERT_EQ(tui::load_hints(path).size(), 0u);
    write(R"({"hints":[1,null,"ok",""]})");
    const auto loaded = tui::load_hints(path);
    ASSERT_EQ(loaded.size(), 1u);
    ASSERT_EQ(loaded[0], std::string("ok"));
    std::remove(path.c_str());
}

TEST(hints_load_the_shipped_file_and_have_a_fallback) {
    const auto loaded = tui::load_hints("hints.json");
    ASSERT(loaded.size() >= 20u);
    ASSERT(tui::default_hints().size() >= 20u);
    ASSERT_EQ(tui::load_hints("no-such-hints-file.json").size(), 0u);
}
