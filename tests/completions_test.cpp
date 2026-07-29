#include <cassert>
#include <iostream>
#include <string>
#include <vector>

// Minimal test framework
#define TEST(name) void name()
#define ASSERT(cond) do { \
    if (!(cond)) { std::cerr << "FAIL: " << #cond << "\n"; failed++; } \
} while(0)
#define ASSERT_EQ(a,b) do { \
    if ((a) != (b)) { std::cerr << "FAIL: " << #a << " == " << #b << "  got: " << (a) << " expected: " << (b) << "\n"; failed++; } \
} while(0)

int failed = 0;

#include "tui/setting_registry.h"

// ── Test: JSON loads and produces expected actions ─────────────────

TEST(test_json_loads_all_commands) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);
    // After loading, the command_completions_ map should have entries.
    // We verify by checking that the help text for known keys is populated.
    std::string h = reg.help_for("detection.loop");
    ASSERT(!h.empty());
}

// ── Test: core actions have help text ──────────────────────────────

TEST(test_core_actions_have_help) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    // These should all have help text from the JSON.
    ASSERT(!reg.help_for("detection.loop").empty());
    ASSERT(!reg.help_for("compression.threshold").empty());
    ASSERT(!reg.help_for("toolfold").empty());
    ASSERT(!reg.help_for("policy.mode").empty());
    ASSERT(!reg.help_for("think").empty());
}

// ── Test: namespace children are indexed ──────────────────────────

TEST(test_namespace_children) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto get_kids = reg.children_of("get");
    ASSERT(!get_kids.empty());
    bool found_think = false;
    for (const auto& k : get_kids)
        if (k == "think") found_think = true;
    ASSERT(found_think);
    auto policy_kids = reg.children_of("policy");
    ASSERT(!policy_kids.empty());
    ASSERT_EQ(policy_kids.size(), 3u);
    // JSON object keys are sorted alphabetically by nlohmann::json (std::map).
    ASSERT_EQ(policy_kids.at(0), "approval");
}

// ── Test: choices are loaded from JSON ─────────────────────────────

TEST(test_choices_loaded) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto ch = reg.choices_for("detection.loop");
    ASSERT(!ch.empty());
    ASSERT_EQ(ch.size(), 3u);
    ASSERT_EQ(ch.at(0), "on");
    ASSERT_EQ(ch.at(1), "off");
    ASSERT_EQ(ch.at(2), "toggle");
}

// ── Test: ranges are loaded from JSON ──────────────────────────────

TEST(test_ranges_loaded) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    double lo, hi;
    bool ok = reg.range_for("compression.threshold", lo, hi);
    ASSERT(ok);
    ASSERT_EQ(lo, 0.1);
    ASSERT_EQ(hi, 1.0);
}

// ── Test: completions from registry ────────────────────────────────

TEST(test_completions_work) {
    tui::SettingRegistry reg;
    reg.add({"detection.loop", "", "", tui::Setting::Choice, {"on","off","toggle"}});
    reg.add({"detection.duplicate", "", "", tui::Setting::Choice, {"on","off","toggle"}});
    reg.add({"compression.threshold", "", "", tui::Setting::Float, {}, 0.1, 1.0});

    // Empty prefix → namespaces only (marked with trailing /)
    auto all = reg.complete("");
    bool has_detection = false, has_compression = false;
    for (const auto& s : all) {
        if (s == "detection") has_detection = true;
        if (s == "compression") has_compression = true;
    }
    ASSERT(has_detection);
    ASSERT(has_compression);

    // "det" → detection only
    auto det = reg.complete("det");
    ASSERT_EQ(det.size(), 1u);
    ASSERT_EQ(det[0], "detection");

    // "detection." → loop duplicate
    auto sub = reg.complete("detection.");
    ASSERT_EQ(sub.size(), 2u);
    bool has_loop = false, has_dup = false;
    for (const auto& s : sub) {
        if (s == "loop") has_loop = true;
        if (s == "duplicate") has_dup = true;
    }
    ASSERT(has_loop);
    ASSERT(has_dup);
}

// ── Test: JSON-only keys appear in completions (no build_settings entry) ─

TEST(test_json_only_key_in_completions) {
    tui::SettingRegistry reg;
    // Add only ONE setting that's in build_settings.
    reg.add({"detection.loop", "", "", tui::Setting::Choice, {"on","off","toggle"}});
    // Load JSON — this adds keys like "provider", "config", "model" to key_help_.
    bool json_ok = reg.load_completions_json("completions.json");
    ASSERT(json_ok);

    // "prov" should match "provider" from JSON even though it's not in settings_.
    auto prov = reg.complete("prov");
    bool found_provider = false;
    for (const auto& k : prov)
        if (k == "provider" || k == "provider/") found_provider = true;
    ASSERT(found_provider);

    // "mod" should match "model" from JSON.
    auto mod = reg.complete("mod");
    bool found_model = false;
    for (const auto& k : mod)
        if (k == "model" || k == "model/") found_model = true;
    ASSERT(found_model);

    // "conf" should match "config" from JSON.
    auto conf = reg.complete("conf");
    bool found_config = false;
    for (const auto& k : conf)
        if (k == "config" || k == "config/") found_config = true;
    ASSERT(found_config);
}

// ── Test: missing JSON file ────────────────────────────────────────

TEST(test_missing_json_is_ok) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("nonexistent.json");
    ASSERT(!ok);  // should return false, not crash
}

// ── Test: empty completions after no settings ──────────────────────

TEST(test_empty_registry) {
    tui::SettingRegistry reg;
    auto all = reg.complete("");
    ASSERT(all.empty());
    auto d = reg.complete("d");
    ASSERT(d.empty());
}

int main() {
    test_json_loads_all_commands();
    test_core_actions_have_help();
    test_choices_loaded();
    test_ranges_loaded();
    test_completions_work();
    test_missing_json_is_ok();
    test_empty_registry();
    test_json_only_key_in_completions();
    test_namespace_children();

    std::cout << (failed ? "FAILED" : "ALL PASSED")
              << " (" << failed << " failures)\n";
    return failed;
}
