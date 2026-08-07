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

#include "tui/drawer_rows.h"
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
    // Full-path namespaces: the set side is queried separately from get.
    auto set_kids = reg.children_of("set.policy");
    REQUIRE_NONEMPTY(set_kids);
    ASSERT_EQ(set_kids.size(), 4u);
    ASSERT_EQ(set_kids.front(), "approval");
    auto get_kids2 = reg.children_of("get.policy");
    REQUIRE_NONEMPTY(get_kids2);
    ASSERT_EQ(get_kids2.size(), 4u);
    // Dotted key resolution keeps /get <dotted> lookups working.
    ASSERT(!reg.help_for("policy.mode").empty());
    ASSERT(!reg.man_for("policy.mode").empty());
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

// ── Test: completions from registry (full-path namespace queries) ──

TEST(test_completions_work) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);

    // Empty prefix → top-level command keys only.
    auto all = reg.complete("");
    bool has_get = false, has_set = false;
    for (const auto& s : all) {
        if (s == "get") has_get = true;
        if (s == "set") has_set = true;
        ASSERT(s.find('.') == std::string::npos);
    }
    ASSERT(has_get);
    ASSERT(has_set);

    // "get" → its children paths.
    auto get_kids = reg.complete("get");
    bool has_model = false;
    for (const auto& s : get_kids)
        if (s == "get.model") has_model = true;
    ASSERT(has_model);

    // "get.detection" → the two leaf paths.
    auto sub = reg.complete("get.detection");
    ASSERT_EQ(sub.size(), 2u);
    bool has_loop = false, has_dup = false;
    for (const auto& s : sub) {
        if (s == "get.detection.loop") has_loop = true;
        if (s == "get.detection.duplicate") has_dup = true;
    }
    ASSERT(has_loop);
    ASSERT(has_dup);
}

// ── Test: JSON-only keys appear in completions (no build_settings entry) ──

TEST(test_json_only_key_in_completions) {
    tui::SettingRegistry reg;
    bool json_ok = reg.load_completions_json("completions.json");
    ASSERT(json_ok);

    // "provider", "model", "config" are get-namespace children from JSON.
    auto get_kids = reg.complete("get");
    bool found_provider = false, found_model = false, found_config = false;
    for (const auto& k : get_kids) {
        if (k == "get.provider") found_provider = true;
        if (k == "get.model") found_model = true;
        if (k == "get.config") found_config = true;
    }
    ASSERT(found_provider);
    ASSERT(found_model);
    ASSERT(found_config);
}

// ── Test: missing JSON file ────────────────────────────────────────

TEST(test_missing_json_is_ok) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("nonexistent.json");
    ASSERT(!ok);  // should return false, not crash
}

// ── 3-level deep completions (full-path namespace queries) ─────────

TEST(test_complete_depth1_prefix) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    // "/get dete" → children of "get", filtered to the "detection" leaf.
    auto r = reg.complete("get");
    bool found = false;
    for (const auto& k : r)
        if (k == "get.detection") found = true;
    REQUIRE_NONEMPTY(r);
    ASSERT(found);
}

TEST(test_complete_depth2_with_dot) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto r = reg.complete("get.detection");
    REQUIRE_NONEMPTY(r);
    ASSERT_EQ(r.size(), 2u);
    ASSERT_EQ(r.front(), "get.detection.duplicate");
    ASSERT_EQ(r[1], "get.detection.loop");
}

TEST(test_complete_depth2_prefix) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    // "/get detection.d" → children of "get.detection" filtered to "duplicate".
    auto r = reg.complete("get.detection");
    REQUIRE_NONEMPTY(r);
    bool found = false;
    for (const auto& k : r)
        if (k == "get.detection.duplicate") found = true;
    ASSERT(found);
}

TEST(test_complete_depth2_policy) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    // "/set policy" → children of "set.policy" (full-path: distinct from get).
    auto r = reg.complete("set.policy");
    REQUIRE_NONEMPTY(r);
    ASSERT_EQ(r.size(), 4u);
    ASSERT_EQ(r.front(), "set.policy.approval");
    ASSERT_EQ(r[1], "set.policy.mode");
    ASSERT_EQ(r[2], "set.policy.rule");
    ASSERT_EQ(r[3], "set.policy.timeout");
}

TEST(test_complete_depth1_compression_namespace) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto r = reg.complete("get.compression");
    REQUIRE_NONEMPTY(r);
    ASSERT_EQ(r.size(), 2u);
    ASSERT_EQ(r.front(), "get.compression.min_turns");
    ASSERT_EQ(r[1], "get.compression.threshold");
}

TEST(test_complete_nonexistent_namespace) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto r = reg.complete("nonexistent");
    ASSERT(r.empty());
    auto r2 = reg.complete("nonexistent.");
    ASSERT(r2.empty());
}

TEST(test_complete_depth1_namespace_matches_at_top) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    // Top-level "compress" command is a root key...
    auto top = reg.complete("");
    bool found_compress = false;
    for (const auto& s : top)
        if (s == "compress") found_compress = true;
    ASSERT(found_compress);
    // ...and the compression namespace lives under get/set.
    auto get_kids = reg.complete("get");
    bool found_compression = false;
    for (const auto& s : get_kids)
        if (s == "get.compression") found_compression = true;
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

    // Level 3: /set policy <TAB> → children of "set.policy" (full path)
    auto policy_kids = reg.children_of("set.policy");
    REQUIRE_NONEMPTY(policy_kids);
    ASSERT_EQ(policy_kids.size(), 4u);
    ASSERT_EQ(policy_kids.front(), "approval");

    // Level 4: /set policy mode <TAB> → children of "set.policy.mode"
    auto mode_kids = reg.children_of("set.policy.mode");
    ASSERT(mode_kids.empty());  // mode is a leaf node, no children

    // Verify help text at each level (full paths).
    ASSERT(!reg.help_for("set.policy").empty());
    ASSERT(!reg.help_for("set.policy.mode").empty());
    ASSERT(!reg.help_for("set.policy.approval").empty());
}

// ── Test: no root /model command remains (FIX-015) ──
// The old root /model node with list|set|probe children is gone; model
// lives under get/set (see test_model_lives_under_get_set).

TEST(test_model_command_is_leaf) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);
    auto subs = reg.subcommands_for("model");
    ASSERT(subs.empty());
    // The root command is no longer in the tree.
    bool root_model = false;
    for (const auto& k : reg.complete(""))
        if (k == "model") root_model = true;
    ASSERT(!root_model);
}

// ── Test: model lives under get/set, not at the root (FIX-015 P2) ──
// Model is a state-modifying accessor: queries at /get model (current, list,
// context), switching at /set model. There is no root /model command.

TEST(test_model_lives_under_get_set) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);
    bool root_model = false;
    for (const auto& k : reg.complete(""))
        if (k == "model") root_model = true;
    ASSERT(!root_model);
    // /get model list|context are documented children.
    auto get_kids = reg.children_of("get.model");
    REQUIRE_NONEMPTY(get_kids);
    ASSERT_EQ(get_kids.size(), 2u);
    bool has_list = false, has_context = false;
    for (const auto& k : get_kids) {
        if (k == "list") has_list = true;
        if (k == "context") has_context = true;
    }
    ASSERT(has_list && has_context);
    // /set model is a documented branch (bare = current model).
    ASSERT(!reg.help_for("set.model").empty());
    ASSERT(!reg.man_for("set.model").empty());
}

// ── Test: /set model completes from feed leaves (FIX-015 P2) ──
// refresh_model_list() merges model ids as value leaves under set.model;
// completion must surface them (the drawer rows and Tab share this path).

TEST(test_set_model_feed_leaves_complete) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);
    nlohmann::json subtree = nlohmann::json::object();
    subtree["set"]["children"]["model"]["children"]["llama3"]["action"] =
        "core.config.set.model.llama3";
    subtree["set"]["children"]["model"]["children"]["llama3"]["help"] = "ctx 8192";
    subtree["set"]["children"]["model"]["children"]["qwen2"]["action"] =
        "core.config.set.model.qwen2";
    reg.merge_completions_json(subtree);
    auto kids = reg.complete("set.model");
    REQUIRE_NONEMPTY(kids);
    ASSERT_EQ(kids.size(), 2u);
    bool has_llama = false, has_qwen = false;
    for (const auto& k : kids) {
        if (k == "set.model.llama3") has_llama = true;
        if (k == "set.model.qwen2") has_qwen = true;
    }
    ASSERT(has_llama && has_qwen);
    ASSERT_EQ(reg.help_for("set.model.llama3"), "ctx 8192");
}

// ── Test: policy rule curation lives in the tree (FIX-015 P3) ──
// The dangerous-command rules are fed into get/set policy rule as value
// leaves (tool names + current level as help), so the permission system is
// visible and editable through the drawer.

TEST(test_policy_rule_branches_documented) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);
    ASSERT(!reg.help_for("get.policy.rule").empty());
    ASSERT(!reg.man_for("set.policy.rule").empty());
    // get.policy and set.policy both document the rule branch.
    bool get_has_rule = false, set_has_rule = false;
    for (const auto& k : reg.children_of("get.policy"))
        if (k == "rule") get_has_rule = true;
    for (const auto& k : reg.children_of("set.policy"))
        if (k == "rule") set_has_rule = true;
    ASSERT(get_has_rule && set_has_rule);
}

TEST(test_policy_rule_feed_leaves) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);
    // The policy feed shape (refresh_policy_feed): tool leaves under both
    // get.policy.rule and set.policy.rule, level info as help text.
    nlohmann::json subtree = nlohmann::json::object();
    subtree["get"]["children"]["policy"]["children"]["rule"]["children"]["bash"]
        ["action"] = "core.config.get.policy.rule.bash";
    subtree["get"]["children"]["policy"]["children"]["rule"]["children"]["bash"]
        ["help"] = "allow (used 3x)";
    subtree["set"]["children"]["policy"]["children"]["rule"]["children"]["bash"]
        ["action"] = "core.config.set.policy.rule.bash";
    subtree["set"]["children"]["policy"]["children"]["rule"]["children"]["bash"]
        ["help"] = "allow (used 3x)";
    subtree["set"]["children"]["policy"]["children"]["rule"]["children"]["search"]
        ["action"] = "core.config.set.policy.rule.search";
    reg.merge_completions_json(subtree);

    // /set policy rule <Tab> completes tool names...
    auto set_kids = reg.complete("set.policy.rule");
    REQUIRE_NONEMPTY(set_kids);
    ASSERT_EQ(set_kids.size(), 2u);
    bool has_bash = false, has_search = false;
    for (const auto& k : set_kids) {
        if (k == "set.policy.rule.bash") has_bash = true;
        if (k == "set.policy.rule.search") has_search = true;
    }
    ASSERT(has_bash && has_search);
    // ...with the current level as inline help.
    ASSERT_EQ(reg.help_for("set.policy.rule.bash"), "allow (used 3x)");
    // /get policy rule shows only tools that have rules.
    auto get_kids = reg.children_of("get.policy.rule");
    REQUIRE_NONEMPTY(get_kids);
    ASSERT_EQ(get_kids.size(), 1u);
    ASSERT_EQ(get_kids.front(), "bash");
}

// ── Test: job kill/read complete from feed leaves (FIX-015 P4) ──
// Job ids become value leaves under job.kill and job.read (state as help),
// replacing the last hardcoded complete_arg lambda.

TEST(test_job_feed_leaves) {
    tui::SettingRegistry reg;
    bool ok = reg.load_completions_json("completions.json");
    ASSERT(ok);
    nlohmann::json subtree = nlohmann::json::object();
    subtree["job"]["children"]["kill"]["children"]["42"]["action"] =
        "core.job.kill.42";
    subtree["job"]["children"]["kill"]["children"]["42"]["help"] = "running";
    subtree["job"]["children"]["kill"]["children"]["43"]["action"] =
        "core.job.kill.43";
    subtree["job"]["children"]["read"]["children"]["42"]["action"] =
        "core.job.read.42";
    reg.merge_completions_json(subtree);

    // /job kill <Tab> completes running jobs...
    auto kill_kids = reg.complete("job.kill");
    REQUIRE_NONEMPTY(kill_kids);
    ASSERT_EQ(kill_kids.size(), 2u);
    bool has_42 = false, has_43 = false;
    for (const auto& k : kill_kids) {
        if (k == "job.kill.42") has_42 = true;
        if (k == "job.kill.43") has_43 = true;
    }
    ASSERT(has_42 && has_43);
    ASSERT_EQ(reg.help_for("job.kill.42"), "running");
    // ...and /job read offers the same job ids.
    auto read_kids = reg.children_of("job.read");
    REQUIRE_NONEMPTY(read_kids);
    ASSERT_EQ(read_kids.front(), "42");
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
    nlohmann::json subtree = nlohmann::json::object();
    subtree["set"]["children"]["model"]["action"] = "core.config.set.model";
    subtree["set"]["children"]["model"]["children"]["m1"]["action"] =
        "core.config.set.model.m1";
    subtree["set"]["children"]["model"]["children"]["m1"]["help"] = "ctx 8192";
    reg.merge_completions_json(subtree);

    // The set side sees the merged leaves...
    auto set_kids = reg.children_of("set.model");
    ASSERT_EQ(set_kids.size(), 1u);
    ASSERT_EQ(set_kids.front(), "m1");
    ASSERT(!reg.help_for("set.model.m1").empty());
    // ...while the get side of the same namespace stays untouched (its static
    // list/context children are not joined by the set-side feed leaves).
    auto get_kids = reg.children_of("get.model");
    ASSERT_EQ(get_kids.size(), 2u);
    ASSERT(!reg.help_for("get.model").empty());
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
    nlohmann::json subtree = nlohmann::json::object();
    subtree["mcp"]["action"] = "core.mcp";
    subtree["mcp"]["help"] = "Manage MCP servers.";
    subtree["mcp"]["man"] = "Live MCP tools appear under their server name.";
    subtree["mcp"]["children"]["live_srv"]["action"] = "mcp.live_srv";
    subtree["mcp"]["children"]["live_srv"]["children"]["do_thing"]["action"] =
        "mcp.live_srv.do_thing";
    subtree["mcp"]["children"]["live_srv"]["children"]["do_thing"]["help"] =
        "does the thing";
    reg.merge_completions_json(subtree);

    // Static children survive in the tree alongside the live branch.
    auto tree = reg.command_tree();  // mutable copy: operator[] never throws
    const auto& mcp = tree["commands"]["mcp"];
    ASSERT(mcp["action"] == "core.mcp");
    ASSERT(mcp.contains("children"));
    const auto& children = mcp["children"];
    ASSERT(children.contains("list"));
    ASSERT(children.contains("live_srv"));
    ASSERT(children["live_srv"]["children"].contains("do_thing"));
}

TEST(test_feed_leaves_visible_after_merge) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    // Simulate the provider feed merge (the startup wiring must not wipe it).
    nlohmann::json subtree;
    subtree["set"]["children"]["provider"]["children"]["zz_feed_provider"]
        ["action"] = "core.config.set.provider.zz_feed_provider";
    subtree["set"]["children"]["provider"]["children"]["zz_feed_provider"]
        ["help"] = "feed-provider help text";
    reg.merge_completions_json(subtree);

    auto kids = reg.children_of("set.provider");
    bool found = false;
    for (const auto& k : kids)
        if (k == "zz_feed_provider") found = true;
    ASSERT(found);
    ASSERT(!reg.help_for("set.provider.zz_feed_provider").empty());
    // The completion path must surface it too.
    auto completions = reg.complete("set.provider");
    bool found2 = false;
    for (const auto& c : completions)
        if (c == "set.provider.zz_feed_provider") found2 = true;
    ASSERT(found2);
}

// ── Test: drawer rows = children + short description (the contract) ──

TEST(test_drawer_rows_children_and_help) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto rows = tui::drawer_rows("/get model l", reg);
    bool found = false;
    for (const auto& r : rows)
        if (r.find("list") != std::string::npos &&
            r.find("all models") != std::string::npos)
            found = true;
    ASSERT(found);
}

// ── Test: no-space branch completes top-level names from the tree ──

TEST(test_complete_top_level_from_tree) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    auto top = reg.complete("");
    bool has_set = false, has_get = false;
    for (const auto& c : top) {
        has_set |= (c == "set");
        has_get |= (c == "get");
    }
    ASSERT(has_set);
    ASSERT(has_get);
}

int main() {
    try {
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
    test_model_lives_under_get_set();
    test_set_model_feed_leaves_complete();
    test_policy_rule_branches_documented();
    test_policy_rule_feed_leaves();
    test_job_feed_leaves();
    test_full_path_namespaces_are_distinct();
    test_merge_preserves_static_tree_children();
    test_feed_leaves_visible_after_merge();
    test_drawer_rows_children_and_help();
    test_complete_top_level_from_tree();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: unexpected exception: " << e.what() << "\n";
        failed++;
    }

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

// ── Test: subagent settings live in the get/set tree ───────────────

TEST(test_subagent_tree_nodes) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    ASSERT(!reg.help_for("subagent.parallel").empty());
    ASSERT(!reg.man_for("subagent.max").empty());
    auto ch = reg.choices_for("subagent.parallel");
    ASSERT_EQ(ch.size(), 3u);
    ASSERT_EQ(ch[0], "on");
    ASSERT_EQ(ch[1], "off");
    ASSERT_EQ(ch[2], "toggle");
    double lo, hi;
    ASSERT(reg.range_for("subagent.max", lo, hi));
    ASSERT_EQ(lo, 1.0);
    ASSERT_EQ(hi, 16.0);
    auto set_kids = reg.children_of("set.subagent");
    REQUIRE_NONEMPTY(set_kids);
    ASSERT_EQ(set_kids.size(), 2u);
}

// ── Test: reasoning effort lives in the get/set tree ──────────────

TEST(test_reasoning_tree_nodes) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    ASSERT(!reg.help_for("reasoning.effort").empty());
    ASSERT(!reg.man_for("reasoning.effort").empty());
    auto ch = reg.choices_for("reasoning.effort");
    REQUIRE_NONEMPTY(ch);
    ASSERT_EQ(ch[0], "off");
    ASSERT_EQ(ch[1], "low");
    ASSERT_EQ(ch[2], "medium");
    ASSERT_EQ(ch[3], "high");
}

// ── Test: provider lives in the set tree (feed merges saved names) ──

TEST(test_provider_tree_nodes) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    ASSERT(!reg.help_for("set.provider").empty());
    ASSERT(!reg.man_for("set.provider").empty());
}

// ── Test: no hardcoded command paths — mcp/learn are tree nodes ────

TEST(test_get_mcp_learn_tree_nodes) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    ASSERT(!reg.help_for("get.mcp").empty());
    ASSERT(!reg.help_for("get.learn").empty());
    ASSERT(!reg.man_for("mcp").empty());
}

// ── Test: get provider list node ─────────────────────────────────

TEST(test_get_provider_list_node) {
    tui::SettingRegistry reg;
    reg.load_completions_json("completions.json");
    ASSERT(!reg.help_for("provider.list").empty());
}

// ── Test: feed leaves are visible to the drawer/completion queries ──

