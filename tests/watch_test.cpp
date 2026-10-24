// Hermetic tests for the watch plugin: records/store, process and PR profiles,
// the one-shot observer, owner routing, the plugin's prompt/tool unwind, and
// the TUI report queue. No network, no real gh.

#include "plugins/watch/watch_observer.h"
#include "plugins/watch/watch_plugin.h"
#include "plugins/watch/watch_profiles.h"
#include "plugins/watch/watch_service.h"
#include "plugins/watch/watch_settings.h"
#include "plugins/watch/watch_store.h"
#include "plugins/watch/watch_tools.h"
#include "plugins/watch/watch_types.h"

#include "agent/events.h"
#include "agent/plugin_runtime.h"
#include "agent/run_scope.h"
#include "tui/run_registry.h"

#include "test_util.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace agent;
using namespace agent::plugins;

namespace {

namespace fs = std::filesystem;

class ScratchEnv {
public:
    explicit ScratchEnv(const std::string& name)
        : root_(fs::temp_directory_path() / ("amber_watch_test_" + name)) {
        fs::remove_all(root_);
        fs::create_directories(root_);
        setenv("XDG_CONFIG_HOME", (root_ / "config").c_str(), 1);
        setenv("XDG_STATE_HOME", (root_ / "state").c_str(), 1);
        setenv("XDG_CACHE_HOME", (root_ / "cache").c_str(), 1);
    }
    ~ScratchEnv() {
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_STATE_HOME");
        unsetenv("XDG_CACHE_HOME");
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    ScratchEnv(const ScratchEnv&) = delete;
    ScratchEnv& operator=(const ScratchEnv&) = delete;

    std::string path(const std::string& leaf) const { return (root_ / leaf).string(); }

private:
    fs::path root_;
};

class FakeRunner final : public CommandRunner {
public:
    std::function<Result(const std::string&, const std::string&, int, const CancellationToken&)>
        handler;
    mutable std::mutex mtx;
    mutable std::vector<std::string> commands;

    Result run(const std::string& command, const std::string& cwd, int timeout_s,
               const CancellationToken& cancel) override {
        {
            std::scoped_lock lk(mtx);
            commands.push_back(command);
        }
        if (!handler)
            return {std::string(), 0, false, false};
        return handler(command, cwd, timeout_s, cancel);
    }
};

class StubTaskTool final : public Tool {
public:
    std::string name() const noexcept override { return "task"; }
    std::string description() const noexcept override { return "delegate a task"; }
    json parameters_schema() const override { return json::object(); }
    ToolResult execute(const json&) const override { return {true, "", "", json::object()}; }
};

class FakeObserver final : public ObserverModel {
public:
    std::string reply;
    int delay_ms = 0;
    mutable std::atomic<int> calls{0};
    mutable std::atomic<int> active{0};
    mutable std::atomic<int> max_active{0};
    mutable std::mutex mtx;
    std::string last_brief;
    agent::UsageTotals usage;

    std::string complete(const std::string&, const std::string& brief,
                         const CancellationToken&) override {
        ++calls;
        const int now = ++active;
        int observed_max = max_active.load();
        while (now > observed_max && !max_active.compare_exchange_weak(observed_max, now)) {
        }
        {
            std::scoped_lock lk(mtx);
            last_brief = brief;
        }
        if (delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        --active;
        return reply;
    }

    agent::UsageTotals take_usage() override {
        std::scoped_lock lk(mtx);
        agent::UsageTotals out = usage;
        usage = {};
        return out;
    }
};

std::optional<WatchRecord> wait_terminal(WatchService& service, const std::string& id,
                                         int seconds = 5) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        auto record = service.get(id);
        if (record && watch_state_terminal(record->state))
            return record;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return service.get(id);
}

WatchStart process_options(const std::string& command, const std::string& label = "test watch") {
    WatchStart options;
    options.profile = "process";
    options.command = command;
    options.label = label;
    options.deadline_s = 5;
    options.interval_s = 1;
    options.owner_id = "7";
    return options;
}

std::string checks_json(const std::string& state, const std::string& link = "") {
    return "[{\"name\":\"build\",\"state\":\"" + state + "\",\"link\":\"" + link + "\"}]";
}

const char* kFailureReport =
    R"({"headline":"build failed on E0308","observed":["error[E0308] at src/foo.rs:42"],"likely_cause":"type mismatch","next_action":"fix src/foo.rs:42","confidence":"high","needs_attention":true,"journal_update":"same E0308 as before"})";

} // namespace

TEST(watch_store_round_trips_records_and_reports) {
    ScratchEnv env("store");
    WatchStore store(env.path("watch"));
    WatchRecord record;
    record.id = "watch_1";
    record.owner_id = "7";
    record.label = "unit";
    record.profile = "process";
    record.state = WatchState::Running;
    record.journal.push_back("queued");
    ASSERT_TRUE(store.save_record(record));

    const auto loaded = store.load_records();
    ASSERT_EQ(loaded.size(), std::size_t{1});
    ASSERT_EQ(loaded[0].id, std::string("watch_1"));
    ASSERT_EQ(loaded[0].owner_id, std::string("7"));
    ASSERT_TRUE(loaded[0].state == WatchState::Running);

    WatchReport report;
    report.watch_id = "watch_1";
    report.report_id = "watch_1:1";
    report.state = WatchState::Failed;
    report.headline = "failed";
    report.observed.push_back("boom");
    ASSERT_TRUE(store.save_report(report));
    WatchReport back;
    ASSERT_TRUE(store.load_report("watch_1", back));
    ASSERT_EQ(back.headline, std::string("failed"));
    ASSERT_EQ(back.observed.size(), std::size_t{1});
}

TEST(watch_process_success_is_deterministic_and_skips_observer) {
    ScratchEnv env("process_ok");
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string&, const std::string&, int, const CancellationToken&) {
        return CommandRunner::Result{"ok\n", 0, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    observer->reply = kFailureReport;
    EventBus bus;
    std::mutex event_mtx;
    std::vector<WatchReportEvent> events;
    auto sub = Events(bus).subscribe<WatchReportEvent>([&](const WatchReportEvent& ev) {
        std::scoped_lock lk(event_mtx);
        events.push_back(ev);
    });

    {
        WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", &bus);
        std::string err;
        const std::string id = service.start(process_options("true"), err);
        ASSERT_TRUE(err.empty());
        ASSERT_TRUE(!id.empty());
        auto record = wait_terminal(service, id);
        ASSERT_TRUE(record.has_value());
        ASSERT_TRUE(record->state == WatchState::Succeeded);
        const auto report = service.report(id);
        ASSERT_TRUE(report.has_value());
        ASSERT_TRUE(report->headline.find("succeeded") != std::string::npos);
        ASSERT_EQ(observer->calls.load(), 0);
    }
    std::scoped_lock lk(event_mtx);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].owner_id, std::string("7"));
    ASSERT_EQ(events[0].state, std::string("succeeded"));
}

TEST(watch_process_failure_uses_observer_and_publishes_owner) {
    ScratchEnv env("process_fail");
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string&, const std::string&, int, const CancellationToken&) {
        return CommandRunner::Result{"error: boom\n", 2, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    observer->reply = kFailureReport;
    EventBus bus;
    std::mutex event_mtx;
    std::vector<WatchReportEvent> events;
    auto sub = Events(bus).subscribe<WatchReportEvent>([&](const WatchReportEvent& ev) {
        std::scoped_lock lk(event_mtx);
        events.push_back(ev);
    });

    WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", &bus);
    std::string err;
    const std::string id = service.start(process_options("false"), err);
    ASSERT_TRUE(err.empty());
    auto record = wait_terminal(service, id);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->state == WatchState::Failed);
    const auto report = service.report(id);
    ASSERT_TRUE(report.has_value());
    ASSERT_EQ(report->headline, std::string("build failed on E0308"));
    ASSERT_EQ(observer->calls.load(), 1);
    ASSERT_TRUE(!report->artifact_path.empty());

    std::scoped_lock lk(event_mtx);
    ASSERT_EQ(events.size(), std::size_t{1});
    ASSERT_EQ(events[0].owner_id, std::string("7"));
    ASSERT_TRUE(events[0].needs_attention);
}

TEST(watch_observer_usage_is_attributed_to_the_owner) {
    ScratchEnv env("usage_owner");
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string&, const std::string&, int, const CancellationToken&) {
        return CommandRunner::Result{"error: boom\n", 2, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    observer->reply = kFailureReport;
    agent::UsageTotals spent;
    spent.prompt_tokens = 1200;
    spent.completion_tokens = 30;
    spent.measured = true;
    observer->usage = spent;
    EventBus bus;

    WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", &bus);
    std::string err;
    const std::string id = service.start(process_options("false"), err);
    ASSERT_TRUE(err.empty());
    ASSERT_TRUE(wait_terminal(service, id).has_value());

    const auto used = service.take_usage();
    ASSERT_EQ(used.size(), std::size_t{1});
    ASSERT_EQ(used.at("7").prompt_tokens, 1200L);
    ASSERT_EQ(used.at("7").completion_tokens, 30L);
    ASSERT(used.at("7").measured);
    ASSERT(service.take_usage().empty()); // drained once
}

TEST(watch_report_ack_and_reemit_survive_restart) {
    ScratchEnv env("ack");
    const std::string root = env.path("watch");
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string&, const std::string&, int, const CancellationToken&) {
        return CommandRunner::Result{"done", 0, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    EventBus bus;
    std::mutex mtx;
    int events = 0;
    auto sub = Events(bus).subscribe<WatchReportEvent>([&](const WatchReportEvent&) {
        std::scoped_lock lk(mtx);
        ++events;
    });

    std::string id;
    {
        WatchService service(runner, observer, WatchStore(root), "observer", &bus);
        std::string err;
        id = service.start(process_options("true"), err);
        ASSERT_TRUE(err.empty());
        ASSERT_TRUE(wait_terminal(service, id).has_value());
        ASSERT_EQ(service.pending_reports().size(), std::size_t{1});
        service.ack(id);
        ASSERT_TRUE(service.pending_reports().empty());
    }
    {
        // A delivered report is not re-emitted.
        WatchService restarted(runner, observer, WatchStore(root), "observer", &bus);
        restarted.reemit_pending();
        std::scoped_lock lk(mtx);
        ASSERT_EQ(events, 1); // the live event only
    }

    // A report that was never acked is re-emitted after the host subscribes.
    {
        WatchService service(runner, observer, WatchStore(root), "observer", &bus);
        std::string err;
        const std::string second = service.start(process_options("true"), err);
        ASSERT_TRUE(err.empty());
        ASSERT_TRUE(wait_terminal(service, second).has_value());
        // Do not ack.
    }
    {
        WatchService restarted(runner, observer, WatchStore(root), "observer", &bus);
        ASSERT_EQ(restarted.pending_reports().size(), std::size_t{1});
        restarted.reemit_pending();
        std::scoped_lock lk(mtx);
        ASSERT_EQ(events, 3); // live second + re-emitted second
        restarted.ack(restarted.pending_reports().front().watch_id);
        ASSERT_TRUE(restarted.pending_reports().empty());
    }
}

TEST(watch_abandoned_owner_is_not_reemitted) {
    ScratchEnv env("abandon");
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string&, const std::string&, int, const CancellationToken&) {
        return CommandRunner::Result{"done", 0, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    EventBus bus;
    std::mutex mtx;
    int events = 0;
    auto sub = Events(bus).subscribe<WatchReportEvent>([&](const WatchReportEvent&) {
        std::scoped_lock lk(mtx);
        ++events;
    });
    std::string id;
    {
        WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", &bus);
        std::string err;
        id = service.start(process_options("true"), err);
        ASSERT_TRUE(err.empty());
        ASSERT_TRUE(wait_terminal(service, id).has_value());
        service.abandon_owner("7");
        ASSERT_TRUE(service.pending_reports().empty());
    }
    {
        WatchService restarted(runner, observer, WatchStore(env.path("watch")), "observer", &bus);
        restarted.reemit_pending();
        std::scoped_lock lk(mtx);
        ASSERT_EQ(events, 1); // live only; abandoned is not re-emitted
    }
}

TEST(watch_job_runner_runs_a_real_process) {
    ScratchEnv env("real_process");
    JobService jobs;
    auto runner = std::make_shared<JobCommandRunner>(jobs);
    auto observer = std::make_shared<FakeObserver>();
    WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", nullptr);
    std::string err;
    const std::string id =
        service.start(process_options("printf 'hello from watch\\n'; exit 3"), err);
    ASSERT_TRUE(err.empty());
    auto record = wait_terminal(service, id, 10);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->state == WatchState::Failed);
    const auto report = service.report(id);
    ASSERT_TRUE(report.has_value());
    ASSERT_TRUE(!report->artifact_path.empty());
    bool saw_output = false;
    for (const auto& line : report->observed)
        saw_output = saw_output || line.find("hello from watch") != std::string::npos;
    ASSERT_TRUE(saw_output);
}

TEST(watch_cancel_stops_a_blocked_runner) {
    ScratchEnv env("cancel");
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string&, const std::string&, int, const CancellationToken& c) {
        for (int i = 0; i < 500 && !c.is_requested(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return CommandRunner::Result{"partial", 0, false, c.is_requested()};
    };
    auto observer = std::make_shared<FakeObserver>();
    WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", nullptr);
    std::string err;
    const std::string id = service.start(process_options("sleep"), err);
    ASSERT_TRUE(err.empty());

    // Let the worker enter the runner before cancelling.
    for (int i = 0; i < 100; ++i) {
        auto record = service.get(id);
        if (record && record->state == WatchState::Running)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(service.cancel(id));
    auto record = wait_terminal(service, id);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->state == WatchState::Cancelled);
}

TEST(watch_cancel_owner_stops_a_window_watch) {
    ScratchEnv env("cancel_owner");
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string&, const std::string&, int, const CancellationToken& c) {
        for (int i = 0; i < 500 && !c.is_requested(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return CommandRunner::Result{"partial", 0, false, c.is_requested()};
    };
    auto observer = std::make_shared<FakeObserver>();
    WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", nullptr);
    std::string err;
    const std::string id = service.start(process_options("sleep"), err);
    ASSERT_TRUE(err.empty());
    for (int i = 0; i < 100; ++i) {
        auto record = service.get(id);
        if (record && record->state == WatchState::Running)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    service.cancel_owner("7");
    auto record = wait_terminal(service, id);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->state == WatchState::Cancelled);
}

TEST(watch_url_change_uses_observer_and_artifact) {
    ScratchEnv env("url");
    auto runner = std::make_shared<FakeRunner>();
    int fetches = 0;
    runner->handler = [&fetches](const std::string&, const std::string&, int,
                                 const CancellationToken&) {
        ++fetches;
        if (fetches == 1 || fetches == 2)
            return CommandRunner::Result{"one\n", 0, false, false};
        return CommandRunner::Result{"one\ntwo\n", 0, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    observer->reply = kFailureReport;
    WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", nullptr);
    WatchStart options;
    options.profile = "url";
    options.url = "https://example.test/status";
    options.owner_id = "7";
    options.interval_s = 1;
    options.deadline_s = 5;
    std::string err;
    const std::string id = service.start(options, err);
    ASSERT_TRUE(err.empty());
    auto record = wait_terminal(service, id, 10);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->state == WatchState::Succeeded);
    const auto report = service.report(id);
    ASSERT_TRUE(report.has_value());
    ASSERT_EQ(observer->calls.load(), 1);
    ASSERT_TRUE(!report->artifact_path.empty());
    bool saw_diff = false;
    for (const auto& line : report->observed)
        saw_diff = saw_diff || line.find("first difference") != std::string::npos;
    ASSERT_TRUE(saw_diff);
    ASSERT_TRUE(safe_url("https://example.test/a?b=c"));
    ASSERT_FALSE(safe_url("file:///etc/passwd"));
    ASSERT_FALSE(safe_url("https://example.test/a b"));
}

TEST(watch_retention_keeps_the_newest_terminal_records) {
    ScratchEnv env("retention");
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string&, const std::string&, int, const CancellationToken&) {
        return CommandRunner::Result{"done", 0, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    WatchSettings settings;
    settings.retention = 1;
    WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", nullptr,
                         settings);
    std::string err;
    const std::string first = service.start(process_options("true", "first"), err);
    ASSERT_TRUE(err.empty());
    ASSERT_TRUE(wait_terminal(service, first).has_value());
    const std::string second = service.start(process_options("true", "second"), err);
    ASSERT_TRUE(err.empty());
    ASSERT_TRUE(wait_terminal(service, second).has_value());
    const auto records = service.list();
    ASSERT_EQ(records.size(), std::size_t{1});
    ASSERT_EQ(records[0].id, second);
    ASSERT_FALSE(service.get(first).has_value());
}

TEST(watch_output_flood_is_bounded) {
    ScratchEnv env("flood");
    JobService jobs;
    auto runner = std::make_shared<JobCommandRunner>(jobs);
    auto observer = std::make_shared<FakeObserver>();
    WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", nullptr);
    std::string err;
    const std::string id =
        service.start(process_options("head -c 2097152 /dev/zero | tr '\\0' x", "flood"), err);
    ASSERT_TRUE(err.empty());
    auto record = wait_terminal(service, id, 15);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->state == WatchState::Succeeded);
    const auto report = service.report(id);
    ASSERT_TRUE(report.has_value());
    ASSERT_TRUE(!report->artifact_path.empty());
    const auto size = fs::file_size(report->artifact_path);
    ASSERT_TRUE(size <= 1024u * 1024u);
    std::size_t observed_bytes = 0;
    for (const auto& line : report->observed)
        observed_bytes += line.size();
    ASSERT_TRUE(observed_bytes < 16u * 1024u);
}

TEST(watch_observers_serialize_across_concurrent_failures) {
    ScratchEnv env("serial");
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string&, const std::string&, int, const CancellationToken&) {
        return CommandRunner::Result{"boom", 1, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    observer->reply = kFailureReport;
    observer->delay_ms = 80;
    WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", nullptr);
    std::string err;
    const std::string first = service.start(process_options("false", "first"), err);
    ASSERT_TRUE(err.empty());
    const std::string second = service.start(process_options("false", "second"), err);
    ASSERT_TRUE(err.empty());
    ASSERT_TRUE(wait_terminal(service, first).has_value());
    ASSERT_TRUE(wait_terminal(service, second).has_value());
    ASSERT_EQ(observer->calls.load(), 2);
    ASSERT_EQ(observer->max_active.load(), 1);
}

TEST(watch_settings_defaults_and_limits) {
    ScratchEnv env("settings");
    const std::string path = env.path("watch.conf");
    {
        std::ofstream out(path);
        out << "observer=off\nmax_running=7\ninterval_s=3\ndeadline_s=9\nretention=4\n"
               "retention_days=2\n";
    }
    const auto settings = load_watch_settings(path);
    ASSERT_FALSE(settings.observer);
    ASSERT_EQ(settings.max_running, 7);
    ASSERT_EQ(settings.interval_s, 3);
    ASSERT_EQ(settings.deadline_s, 9);
    ASSERT_EQ(settings.retention, 4);
    ASSERT_EQ(settings.retention_days, 2);
    ASSERT_TRUE(load_watch_settings(env.path("missing.conf")).observer);
}

TEST(watch_pr_success_needs_no_observer) {
    ScratchEnv env("pr_ok");
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string& command, const std::string&, int,
                         const CancellationToken&) {
        if (command.find("gh pr view") != std::string::npos)
            return CommandRunner::Result{R"({"headRefOid":"abc123"})", 0, false, false};
        if (command.find("gh pr checks") != std::string::npos)
            return CommandRunner::Result{checks_json("SUCCESS"), 0, false, false};
        return CommandRunner::Result{"", 0, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", nullptr);
    WatchStart options;
    options.profile = "pr";
    options.pr = "312";
    options.owner_id = "7";
    options.deadline_s = 3;
    options.interval_s = 1;
    std::string err;
    const std::string id = service.start(options, err);
    ASSERT_TRUE(err.empty());
    auto record = wait_terminal(service, id);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->state == WatchState::Succeeded);
    const auto report = service.report(id);
    ASSERT_TRUE(report.has_value());
    ASSERT_TRUE(report->headline.find("passed") != std::string::npos);
    ASSERT_EQ(observer->calls.load(), 0);
    ASSERT_TRUE(checks_terminal(checks_json("SUCCESS")));
    ASSERT_TRUE(!checks_terminal(checks_json("PENDING")));
}

TEST(watch_pr_failure_fetches_log_and_uses_observer) {
    ScratchEnv env("pr_fail");
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string& command, const std::string&, int,
                         const CancellationToken&) {
        if (command.find("gh pr view") != std::string::npos)
            return CommandRunner::Result{R"({"headRefOid":"abc123"})", 0, false, false};
        if (command.find("gh pr checks") != std::string::npos)
            // Real gh exits non-zero for failed checks; the JSON is still the
            // status the profile must read.
            return CommandRunner::Result{
                checks_json("FAILURE", "https://github.com/x/y/actions/runs/42/job/1"), 1, false,
                false};
        if (command.find("gh run view 42") != std::string::npos)
            return CommandRunner::Result{"error[E0308] at src/foo.rs:42\n", 1, false, false};
        return CommandRunner::Result{"", 1, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    observer->reply = kFailureReport;
    WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", nullptr);
    WatchStart options;
    options.profile = "pr";
    options.pr = "312";
    options.owner_id = "7";
    options.deadline_s = 3;
    options.interval_s = 1;
    std::string err;
    const std::string id = service.start(options, err);
    ASSERT_TRUE(err.empty());
    auto record = wait_terminal(service, id);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->state == WatchState::Failed);
    const auto report = service.report(id);
    ASSERT_TRUE(report.has_value());
    ASSERT_EQ(observer->calls.load(), 1);
    ASSERT_TRUE(!report->artifact_path.empty());
    ASSERT_TRUE(report->headline.find("E0308") != std::string::npos);
    bool saw_failed_check = false;
    for (const auto& line : report->observed)
        saw_failed_check =
            saw_failed_check || line.find("failed check: build") != std::string::npos;
    ASSERT_TRUE(saw_failed_check);
}

TEST(watch_pr_new_head_starts_a_new_attempt) {
    ScratchEnv env("pr_attempt");
    auto runner = std::make_shared<FakeRunner>();
    int meta_calls = 0;
    int check_calls = 0;
    runner->handler = [&meta_calls, &check_calls](const std::string& command, const std::string&,
                                                  int, const CancellationToken&) {
        if (command.find("gh pr view") != std::string::npos) {
            ++meta_calls;
            const std::string sha = meta_calls <= 2 ? "aaaa" : "bbbb";
            return CommandRunner::Result{R"({"headRefOid":")" + sha + R"("})", 0, false, false};
        }
        if (command.find("gh pr checks") != std::string::npos) {
            ++check_calls;
            return CommandRunner::Result{
                check_calls == 1
                    ? checks_json("PENDING")
                    : checks_json("FAILURE", "https://github.com/x/y/actions/runs/42/job/1"),
                0, false, false};
        }
        if (command.find("gh run view 42") != std::string::npos)
            return CommandRunner::Result{"error: still broken\n", 1, false, false};
        if (command.find("gh pr diff") != std::string::npos)
            return CommandRunner::Result{"src/foo.rs\n", 0, false, false};
        return CommandRunner::Result{"", 1, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    observer->reply = kFailureReport;
    WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", nullptr);
    WatchStart options;
    options.profile = "pr";
    options.pr = "312";
    options.owner_id = "7";
    options.interval_s = 1;
    options.deadline_s = 5;
    std::string err;
    const std::string id = service.start(options, err);
    ASSERT_TRUE(err.empty());
    auto record = wait_terminal(service, id, 10);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->state == WatchState::Failed);
    ASSERT_EQ(record->attempt, 2);
    ASSERT_TRUE(!record->previous_summary.empty());
    const auto report = service.report(id);
    ASSERT_TRUE(report.has_value());
    ASSERT_EQ(report->attempt, 2);
    bool saw_attempt = false;
    bool saw_previous = false;
    bool saw_changed = false;
    for (const auto& line : report->observed) {
        saw_attempt = saw_attempt || line == "attempt: 2";
        saw_previous = saw_previous || line.rfind("previous attempt:", 0) == 0;
        saw_changed = saw_changed || line.find("changed files") != std::string::npos;
    }
    ASSERT_TRUE(saw_attempt);
    ASSERT_TRUE(saw_previous);
    ASSERT_TRUE(saw_changed);
}

TEST(watch_observer_invalid_json_keeps_deterministic_report) {
    ScratchEnv env("bad_json");
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string&, const std::string&, int, const CancellationToken&) {
        return CommandRunner::Result{"boom", 1, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    observer->reply = "not json at all";
    WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", nullptr);
    std::string err;
    const std::string id = service.start(process_options("false"), err);
    ASSERT_TRUE(err.empty());
    auto record = wait_terminal(service, id);
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->state == WatchState::Failed);
    const auto report = service.report(id);
    ASSERT_TRUE(report.has_value());
    ASSERT_TRUE(report->format_error);
    ASSERT_TRUE(report->headline.find("failed") != std::string::npos);
    bool saw_format_note = false;
    for (const auto& line : report->observed)
        saw_format_note = saw_format_note || line.find("not valid JSON") != std::string::npos;
    ASSERT_TRUE(saw_format_note);
}

TEST(watch_pr_argument_and_check_helpers) {
    ASSERT_TRUE(safe_pr_argument("312"));
    ASSERT_TRUE(safe_pr_argument("https://github.com/x/y/pull/312"));
    ASSERT_FALSE(safe_pr_argument("312; rm -rf /"));
    ASSERT_FALSE(safe_pr_argument(""));
    const auto failed = failed_check_names(
        R"([{"name":"build","state":"FAILURE"},{"name":"lint","state":"SUCCESS"}])");
    ASSERT_EQ(failed.size(), std::size_t{1});
    ASSERT_EQ(failed[0], std::string("build"));
}

TEST(watch_plugin_contributes_prompt_and_tool_then_unwinds) {
    ScratchEnv env("plugin");
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginRuntime runtime(tools, cfg, ws);

    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string&, const std::string&, int, const CancellationToken&) {
        return CommandRunner::Result{"ok", 0, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    runtime.add(std::make_shared<WatchPlugin>(runner, observer, env.path("watch")));
    runtime.start();
    ASSERT_TRUE(runtime.status("watch").enabled);
    ASSERT_TRUE(static_cast<bool>(tools.find("watch")));

    std::string system;
    for (const auto& block : runtime.prompts().render_all(PromptPlacement::System))
        system += block + "\n";
    ASSERT_TRUE(system.find("Background watches") != std::string::npos);
    ASSERT_TRUE(system.find("watch") != std::string::npos);
    ASSERT_TRUE(system.find("full worker path") == std::string::npos);
    bool has_watch_command = false;
    for (const auto& entry : runtime.commands().all())
        has_watch_command = has_watch_command || entry.spec.name == "watch";
    ASSERT_TRUE(has_watch_command);
    bool has_watch_segment = false;
    for (const auto& item : runtime.status().items())
        has_watch_segment = has_watch_segment || item.name == "watch";
    ASSERT_TRUE(has_watch_segment);
    ASSERT_TRUE(runtime.panels().find("watch") != nullptr);

    ASSERT_TRUE(runtime.set_state("watch", false));
    ASSERT_FALSE(static_cast<bool>(tools.find("watch")));
    std::string after;
    for (const auto& block : runtime.prompts().render_all(PromptPlacement::System))
        after += block + "\n";
    ASSERT_TRUE(after.find("Background watches") == std::string::npos);
    bool watch_command_after = false;
    for (const auto& entry : runtime.commands().all())
        watch_command_after = watch_command_after || entry.spec.name == "watch";
    ASSERT_FALSE(watch_command_after);
    ASSERT_TRUE(runtime.panels().find("watch") == nullptr);
}

TEST(watch_plugin_delegation_fragment_follows_the_task_tool) {
    ScratchEnv env("plugin_delegate");
    ToolRegistry tools;
    tools.register_tool(std::make_unique<StubTaskTool>(), "test",
                        ToolMeta{"delegating", ToolRole::Delegate});
    Config cfg;
    Workspace ws;
    PluginRuntime runtime(tools, cfg, ws);
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string&, const std::string&, int, const CancellationToken&) {
        return CommandRunner::Result{"ok", 0, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    runtime.add(std::make_shared<WatchPlugin>(runner, observer, env.path("watch")));
    runtime.start();
    std::string system;
    for (const auto& block : runtime.prompts().render_all(PromptPlacement::System))
        system += block + "\n";
    ASSERT_TRUE(system.find("full worker path") != std::string::npos);
    ASSERT_TRUE(runtime.set_state("watch", false));
}

TEST(watch_command_and_panel_read_live_state) {
    ScratchEnv env("commands");
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginRuntime runtime(tools, cfg, ws);
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string&, const std::string&, int, const CancellationToken&) {
        return CommandRunner::Result{"ok", 0, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    runtime.add(std::make_shared<WatchPlugin>(runner, observer, env.path("watch")));
    runtime.start();

    ASSERT_EQ(runtime.settings().get("watch", "max_running"), std::string("32"));
    ASSERT_TRUE(runtime.settings().set("watch", "max_running", "4"));
    ASSERT_EQ(runtime.settings().get("watch", "max_running"), std::string("4"));

    auto tool = tools.find("watch");
    ASSERT_TRUE(static_cast<bool>(tool));
    const auto started = tool->execute(json{{"action", "start"},
                                            {"profile", "process"},
                                            {"command", "true"},
                                            {"label", "command watch"}});
    ASSERT_TRUE(started.ok);
    const std::string id = started.meta.value("id", std::string());
    ASSERT_TRUE(!id.empty());
    ASSERT_TRUE(tool->execute(json{{"action", "wait"}, {"id", id}, {"timeout_s", 5}}).ok);

    const agent::CommandRegistry::Entry* watch = nullptr;
    for (const auto& entry : runtime.commands().all())
        if (entry.spec.name == "watch")
            watch = &entry;
    ASSERT_TRUE(watch != nullptr);
    ASSERT_TRUE(watch->spec.handlers.at("list")("").find(id) != std::string::npos);
    ASSERT_TRUE(watch->spec.handlers.at("show")(id).find("command watch") != std::string::npos);
    ASSERT_TRUE(watch->spec.handlers.at("report")(id).find("command watch") != std::string::npos);
    ASSERT_TRUE(watch->spec.handlers.at("stop")(id).find("cancel") != std::string::npos);

    const agent::PanelSpec* panel = runtime.panels().find("watch");
    ASSERT_TRUE(panel != nullptr);
    const auto lines = panel->lines(80);
    bool saw_id = false;
    for (const auto& line : lines)
        saw_id = saw_id || line.find(id) != std::string::npos;
    ASSERT_TRUE(saw_id);

    ASSERT_TRUE(runtime.set_state("watch", false));
    ASSERT_TRUE(runtime.panels().find("watch") == nullptr);
}

TEST(watch_tool_starts_reports_and_cancels) {
    ScratchEnv env("tool");
    auto runner = std::make_shared<FakeRunner>();
    runner->handler = [](const std::string&, const std::string&, int, const CancellationToken&) {
        return CommandRunner::Result{"tool output", 0, false, false};
    };
    auto observer = std::make_shared<FakeObserver>();
    WatchService service(runner, observer, WatchStore(env.path("watch")), "observer", nullptr);
    auto tool = make_watch_tool(service);

    ASSERT_TRUE(tool->requires_approval(json{{"action", "start"}}));
    ASSERT_FALSE(tool->requires_approval(json{{"action", "list"}}));
    const auto started = tool->execute(json{
        {"action", "start"}, {"profile", "process"}, {"command", "true"}, {"label", "tool watch"}});
    ASSERT_TRUE(started.ok);
    const std::string id = started.meta.value("id", std::string());
    ASSERT_TRUE(!id.empty());
    const auto waited = tool->execute(json{{"action", "wait"}, {"id", id}, {"timeout_s", 5}});
    ASSERT_TRUE(waited.ok);
    ASSERT_EQ(waited.meta.value("state", std::string()), std::string("succeeded"));
    const auto report = tool->execute(json{{"action", "report"}, {"id", id}, {"detail", "full"}});
    ASSERT_TRUE(report.ok);
    ASSERT_TRUE(report.output.find("tool output") != std::string::npos);
    const auto listed = tool->execute(json{{"action", "list"}});
    ASSERT_TRUE(listed.ok);
    ASSERT_TRUE(listed.output.find(id) != std::string::npos);
}

TEST(watch_run_scope_inherits_the_host_owner) {
    std::string owner = "window-3";
    RunScope outer;
    outer.owner_id = &owner;
    ScopedRunScope outer_guard(outer);
    RunScope inner;
    ScopedRunScope inner_guard(inner);
    ASSERT_TRUE(current_run_scope() != nullptr);
    ASSERT_TRUE(current_run_scope()->leaf.owner_id != nullptr);
    ASSERT_EQ(*current_run_scope()->leaf.owner_id, std::string("window-3"));
}

TEST(watch_run_registry_prioritizes_user_prompts_over_reports) {
    tui::RunRegistry registry;
    registry.register_owner("owner-1", 1);
    registry.enqueue(1, "user prompt");
    registry.enqueue_report("owner-1", {"[harness] watch report", "watch-1", "watch-1:1"});
    ASSERT_TRUE(registry.has_pending(1));
    ASSERT_TRUE(registry.has_pending_report(1));
    auto prompt = registry.pop_pending(1);
    ASSERT_TRUE(prompt.has_value());
    ASSERT_EQ(*prompt, std::string("user prompt"));
    auto report = registry.pop_report(1);
    ASSERT_TRUE(report.has_value());
    ASSERT_EQ(report->prompt, std::string("[harness] watch report"));
    ASSERT_EQ(report->watch_id, std::string("watch-1"));
    ASSERT_FALSE(registry.has_pending_report(1));
    registry.erase(1);

    // A late report for a closed window must not resurrect its slot.
    tui::RunRegistry closed;
    closed.enqueue_report("gone", {"[harness] late report", "watch-2", "watch-2:1"});
    ASSERT_FALSE(closed.has_pending_report(99));
}
