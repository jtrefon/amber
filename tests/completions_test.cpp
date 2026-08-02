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

// cppcheck cannot see through the ASSERT macro; a real guard keeps the
// container-access checks provably safe.
#define REQUIRE_NONEMPTY(v)                                                       \
    do {                                                                          \
        if ((v).empty()) {                                                        \
            std::cerr << "FAIL: " #v " empty\n";                                 \
            failed++;                                                             \
            return;                                                               \
        }                                                                         \
    } while (0)

#include "tui/setting_registry.h"

// ── Test: JSON loads and produces expected actions ─────────────────

TEST(test_json_loads_all_commands) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);
    // After loading, the command_completions_ map should have entries.
    // We verify by checking that the help text for known keys is populated.
    std::string h = reg.help_for("detection.loop");
    REQUIRE_NONEMPTY(h);
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
    REQUIRE_NONEMPTY(get_kids);
    bool found_think = false;
    for (const auto& k : get_kids)
        if (k == "think") found_think = true;
    ASSERT(found_think);
    auto policy_kids = reg.children_of("policy");
    REQUIRE_NONEMPTY(policy_kids);
    ASSERT_EQ(policy_kids.size(), 3u);
    // JSON object keys are sorted alphabetically by nlohmann::json (std::map).
    ASSERT_EQ(policy_kids.front(), "approval");
}

// ── Test: choices are loaded from JSON ─────────────────────────────

TEST(test_choices_loaded) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto ch = reg.choices_for("detection.loop");
    REQUIRE_NONEMPTY(ch);
    ASSERT_EQ(ch.size(), 3u);
    ASSERT_EQ(ch.front(), "on");
    ASSERT_EQ(ch[1], "off");
    ASSERT_EQ(ch[2], "toggle");
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
    ASSERT_EQ(det.front(), "detection");

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

// ── 3-level deep completions ────────────────────────────────────────

TEST(test_complete_depth1_prefix) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto r = reg.complete("detec");
    REQUIRE_NONEMPTY(r);
    ASSERT_EQ(r.size(), 1u);
    ASSERT_EQ(r.front(), "detection");
}

TEST(test_complete_depth2_with_dot) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto r = reg.complete("detection.");
    REQUIRE_NONEMPTY(r);
    ASSERT_EQ(r.size(), 2u);
    ASSERT_EQ(r.front(), "duplicate");
    ASSERT_EQ(r[1], "loop");
}

TEST(test_complete_depth2_prefix) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto r = reg.complete("detection.d");
    REQUIRE_NONEMPTY(r);
    ASSERT_EQ(r.size(), 1u);
    ASSERT_EQ(r.front(), "duplicate");
}

TEST(test_complete_depth2_policy) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto r = reg.complete("policy.");
    REQUIRE_NONEMPTY(r);
    ASSERT_EQ(r.size(), 3u);
    ASSERT_EQ(r.front(), "approval");
    ASSERT_EQ(r[1], "mode");
    ASSERT_EQ(r[2], "timeout");
}

TEST(test_complete_depth1_compression_namespace) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto r = reg.complete("compression.");
    REQUIRE_NONEMPTY(r);
    ASSERT_EQ(r.size(), 2u);
    ASSERT_EQ(r.front(), "min_turns");
    ASSERT_EQ(r[1], "threshold");
}

TEST(test_complete_nonexistent_namespace) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto r = reg.complete("nonexistent.");
    ASSERT(r.empty());
}

TEST(test_complete_depth1_namespace_matches_at_top) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto r = reg.complete("comp");
    REQUIRE_NONEMPTY(r);
    // "compression" should appear as a top-level namespace match
    bool found_compression = false;
    for (const auto& s : r)
        if (s == "compression") found_compression = true;
    ASSERT(found_compression);
}

// ── Test: empty completions after no settings ──────────────────────

TEST(test_empty_registry) {
    tui::SettingRegistry reg;
    auto all = reg.complete("");
    ASSERT(all.empty());
    auto d = reg.complete("d");
    ASSERT(d.empty());
}

// ── Test: drawer namespace children at every level ────────────────
// Simulates exactly what draw_drawer() does for /set policy mode

TEST(test_drawer_namespace_levels) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);

    // Level 2: /set <TAB> → children of "set"
    auto set_kids = reg.children_of("set");
    REQUIRE_NONEMPTY(set_kids);
    bool has_policy = false;
    for (const auto& k : set_kids)
        if (k == "policy") has_policy = true;
    ASSERT(has_policy);

    // Level 3: /set policy <TAB> → children of "policy"
    auto policy_kids = reg.children_of("policy");
    REQUIRE_NONEMPTY(policy_kids);
    ASSERT_EQ(policy_kids.size(), 3u);
    ASSERT_EQ(policy_kids.front(), "approval");

    // Level 4: /set policy mode <TAB> → children of "policy.mode"
    auto mode_kids = reg.children_of("policy.mode");
    ASSERT(mode_kids.empty());  // mode is a leaf node, no children

    // Verify help text at each level
    ASSERT(!reg.help_for("policy").empty());
    ASSERT(!reg.help_for("policy.mode").empty());
    ASSERT(!reg.help_for("policy.approval").empty());
}

// ── Test: /model is a pure switch command (no list|set|probe children) ──
// Model discovery/context moved under /get model; switching is /model <name>
// or the /set model drawer. Neither the tree nor the subcommand index may
// still advertise the old list|set|probe verbs.

TEST(test_model_command_is_leaf) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);
    auto subs = reg.subcommands_for("model");
    ASSERT(subs.empty());
    ASSERT(reg.children_of("model").empty());
    // The command is still documented (help text for switching).
    ASSERT(!reg.help_for("model").empty());
}

// ── Test: get/set namespaces are indexed by FULL path (FIX-015 P1) ──
// The stripped-key scheme collapses get.model and set.model into one key, so
// the two sides of the same namespace can never carry different children.
// A feed merge into set.model must not pollute the get side.

TEST(test_full_path_namespaces_are_distinct) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);

    // A feed-style merge: set.model gains dynamic value children.
    nlohmann::json subtree = {
        {"set", {{"children",
                  {{"model",
                    {{"action", "core.config.set.model"},
                     {"children",
                      {{"m1", {{"action", "core.config.set.model.m1"},
                               {"help", "ctx 8192"}}}}}}}}}}}};
    reg.merge_completions_json(subtree);

    // The set side sees the merged leaves...
    auto set_kids = reg.children_of("set.model");
    ASSERT_EQ(set_kids.size(), 1u);
    ASSERT_EQ(set_kids.front(), "m1");
    ASSERT(!reg.help_for("set.model.m1").empty());
    // ...while the get side of the same namespace stays untouched.
    ASSERT(reg.children_of("get.model").empty());
    ASSERT(!reg.help_for("set.model").empty() || !reg.help_for("get.model").empty());
}

// ── Test: merge preserves static fields + children in the TREE (FIX-015 P1) ──
// merge_completions_json replaced tree_["commands"][key] wholesale while the
// index unioned children — a live MCP/plugin merge lost the static children
// from tree-walk dispatch (/mcp list degraded to the raw-arg fallback).

TEST(test_merge_preserves_static_tree_children) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);

    // Same shape mcp_completion_subtree() produces.
    nlohmann::json subtree = {
        {"mcp", {{"action", "core.mcp"},
                 {"help", "Manage MCP servers."},
                 {"man", "Live MCP tools appear under their server name."},
                 {"children",
                  {{"live_srv",
                    {{"action", "mcp.live_srv"},
                     {"children",
                      {{"do_thing", {{"action", "mcp.live_srv.do_thing"},
                                     {"help", "does the thing"}}}}}}}}}}}};
    reg.merge_completions_json(subtree);

    // Static children survive in the tree alongside the live branch.
    const auto& tree = reg.command_tree();
    ASSERT(tree.contains("commands"));
    const auto& mcp = tree["commands"].at("mcp");
    ASSERT_EQ(mcp.at("action").get<std::string>(), "core.mcp");
    ASSERT(mcp.contains("children"));
    ASSERT(mcp["children"].contains("list"));
    ASSERT(mcp["children"].contains("live_srv"));
    ASSERT(mcp["children"].at("live_srv")["children"].contains("do_thing"));
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
    test_complete_depth1_prefix();
    test_complete_depth2_with_dot();
    test_complete_depth2_prefix();
    test_complete_depth2_policy();
    test_complete_depth1_compression_namespace();
    test_complete_nonexistent_namespace();
    test_complete_depth1_namespace_matches_at_top();
    test_drawer_namespace_levels();
    test_model_command_is_leaf();
    test_full_path_namespaces_are_distinct();
    test_merge_preserves_static_tree_children();

    std::cout << (failed ? "FAILED" : "ALL PASSED")
              << " (" << failed << " failures)\n";
    return failed;
}

// ── Test: mcp / skills / plugin / prompt namespaces are documented ──

TEST(test_json_documents_mcp_namespace) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);
    ASSERT(!reg.help_for("mcp.list").empty());
    ASSERT(!reg.man_for("mcp.connect").empty());
    ASSERT(!reg.help_for("core.mcp.trust").empty());  // action-path index
    auto subs = reg.children_of("mcp");
    bool saw_connect = false, saw_trust = false;
    for (const auto& s : subs) {
        if (s == "connect") saw_connect = true;
        if (s == "trust") saw_trust = true;
    }
    ASSERT(saw_connect && saw_trust);
}

TEST(test_json_documents_skills_under_set_and_get) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);
    ASSERT(!reg.help_for("skills.interop").empty());
    ASSERT(!reg.man_for("skills.refresh").empty());
    ASSERT(!reg.help_for("core.config.set.skills.delete").empty());
    // interop has documented choices.
    auto c = reg.choices_for("skills.interop");
    ASSERT(c.size() == 2u);
    ASSERT(c[0] == "on");
    ASSERT(c[1] == "off");
}

TEST(test_json_documents_plugin_and_prompt) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);
    ASSERT(!reg.help_for("plugin.list").empty());
    ASSERT(!reg.man_for("plugin.install").empty());
    ASSERT(!reg.help_for("core.plugin.uninstall").empty());
    auto subs = reg.children_of("plugin");
    bool saw_install = false, saw_uninstall = false, saw_status = false;
    for (const auto& s : subs) {
        if (s == "install") saw_install = true;
        if (s == "uninstall") saw_uninstall = true;
        if (s == "status") saw_status = true;
    }
    ASSERT(saw_install && saw_uninstall && saw_status);
    ASSERT(!reg.help_for("prompt").empty());
}

// ── Test: merging subtrees unions children instead of replacing them ──

TEST(test_json_merge_unions_children) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);
    // A live MCP server branch lands under the existing mcp command.
    nlohmann::json subtree = {
        {"mcp", {{"action", "core.mcp"},
                 {"children",
                  {{"live_srv", {{"action", "mcp.live_srv"},
                                 {"children", {{"do_thing",
                                                {{"action", "mcp.live_srv.do_thing"},
                                                 {"help", "does the thing"}}}}}}}}}}}};
    reg.merge_completions_json(subtree);

    auto subs = reg.children_of("mcp");
    bool has_list = false, has_live = false;
    for (const auto& s : subs) {
        if (s == "list") has_list = true;
        if (s == "live_srv") has_live = true;
    }
    // Static children survive the merge alongside the live branch.
    ASSERT(has_list && has_live);
    ASSERT(!reg.help_for("live_srv.do_thing").empty());
}
