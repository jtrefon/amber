#include "event_router.h"
#include "tui.h"
#include "tool_display.h"
#include "confirm_panel.h"
#include "textutil.h"

#include <utility>

namespace tui {

EventRouter::EventRouter(Tui& tui) : tui_(tui) {}

void EventRouter::push(AgentEvent ev) {
    std::scoped_lock lk(mtx_);
    queue_.push(std::move(ev));
}

std::vector<AgentEvent> EventRouter::pop_all() {
    std::vector<AgentEvent> out;
    std::scoped_lock lk(mtx_);
    while (!queue_.empty()) {
        out.push_back(std::move(queue_.front()));
        queue_.pop();
    }
    return out;
}

bool EventRouter::empty() const {
    std::scoped_lock lk(mtx_);
    return queue_.empty();
}

void EventRouter::clear() {
    std::scoped_lock lk(mtx_);
    std::queue<AgentEvent> empty;
    std::swap(queue_, empty);
}

void EventRouter::join_thread() {
    if (thread_.joinable()) thread_.join();
}

void EventRouter::shutdown_queues(std::queue<AgentEvent>& pending_approvals) {
    std::scoped_lock lk(mtx_);
    shutting_down_ = true;
    deny_all_pending_approvals(queue_);
    deny_all_pending_approvals(pending_approvals);
}

agent::AgentHooks EventRouter::make_hooks(size_t window_id) {
    agent::AgentHooks hooks;
    auto push_event = [this, window_id](AgentEvent ev) {
        if (cancel_.load()) return;
        ev.window_id = window_id;
        std::scoped_lock lk(mtx_);
        queue_.push(std::move(ev));
    };

    hooks.on_reasoning = [push_event](const std::string& d) {
        AgentEvent ev;
        ev.type = AgentEvent::Reasoning;
        ev.text = d;
        push_event(std::move(ev));
    };
    hooks.on_token = [push_event](const std::string& d) {
        AgentEvent ev;
        ev.type = AgentEvent::Token;
        ev.text = d;
        push_event(std::move(ev));
    };
    hooks.on_state = [push_event](agent::RunState s) {
        AgentEvent ev;
        ev.type = AgentEvent::StateChange;
        ev.state = s;
        push_event(std::move(ev));
    };
    hooks.on_stats = [push_event](const agent::Stats& s) {
        AgentEvent ev;
        ev.type = AgentEvent::Stats;
        ev.stats = s;
        push_event(std::move(ev));
    };
    hooks.on_status = [push_event](const std::string& s) {
        AgentEvent ev;
        ev.type = AgentEvent::Status;
        ev.text = s;
        push_event(std::move(ev));
    };
    hooks.on_tool_call = [push_event](const std::string& n, const agent::json& a) {
        AgentEvent ev;
        ev.type = AgentEvent::ToolCall;
        ev.tool_name = n;
        ev.tool_args = a;
        push_event(std::move(ev));
    };
    hooks.on_tool_result = [push_event](const std::string& n,
                                        const agent::ToolResult& r,
                                        const agent::json& a) {
        AgentEvent ev;
        ev.type = AgentEvent::ToolResult;
        ev.tool_name = n;
        ev.tool_args = a;
        ev.tool_result = r;
        push_event(std::move(ev));
    };
    hooks.on_assistant = [push_event](const std::string& s) {
        AgentEvent ev;
        ev.type = AgentEvent::Assistant;
        ev.text = s;
        push_event(std::move(ev));
    };
    hooks.on_approval = [this, window_id](const std::string& name,
                                          const agent::json& args,
                                          const std::string& summary) -> agent::Approval {
        if (cancel_.load()) return agent::Approval::Deny;
        auto p = std::make_shared<std::promise<agent::Approval>>();
        auto f = p->get_future();
        AgentEvent ev;
        ev.type = AgentEvent::Approval;
        ev.text = summary;
        ev.approval_promise = p;
        {
            std::scoped_lock lk(mtx_);
            if (shutting_down_) return agent::Approval::Deny;
            ev.window_id = window_id;
            queue_.push(std::move(ev));
        }
        (void)name;
        (void)args;
        return f.get();
    };

    return hooks;
}


bool EventRouter::drain_events() {
    std::vector<AgentEvent> batch = pop_all();

    if (batch.empty()) return false;

    for (auto& ev : batch) {
        Window* w = route_event(tui_.window_manager_->all(), ev, tui_.window_manager_->active());
        switch (ev.type) {
        case AgentEvent::StateChange:
            tui_.state_ = ev.state;
            if (ev.state == agent::RunState::Idle ||
                ev.state == agent::RunState::Error)
                tui_.running_tool_.clear();
            break;
        case AgentEvent::Reasoning:
            on_reasoning(w, ev);
            break;
        case AgentEvent::Token:
            on_token(w, ev);
            break;
        case AgentEvent::Status:
            // Worker status belongs to the conversation that emitted it.
            if (w) tui_.append_line_to(*w, P_STATUS, ev.text);
            break;
        case AgentEvent::ToolCall:
            on_tool_call(w, ev);
            break;
        case AgentEvent::ToolResult:
            on_tool_result(w, ev);
            break;
        case AgentEvent::Assistant:
            on_assistant(w, ev);
            break;
        case AgentEvent::Stats:
            tui_.stats_ = ev.stats;
            if (ev.stats.prompt_tokens >= 0) {
                tui_.ctx_used_.store(ev.stats.prompt_tokens);
                tui_.live_ctx_offset_ = 0;
            }
            break;
        case AgentEvent::Error:
            on_error(w, ev);
            break;
        case AgentEvent::Done:
            on_done(w, ev);
            break;
        case AgentEvent::CompressResult:
            on_compress_result(w, ev);
            break;
        case AgentEvent::Approval: {
            // While a modal dialog is open the main thread is not in the event
            // loop; queue the approval so we don't nest ncurses dialogs or
            // deadlock the worker on its promise. Resolved in
            // redraw_after_modal() once the modal closes.
            if (tui_.modal_open_) {
                pending_approvals_.push(std::move(ev));
                break;
            }
            resolve_approval(ev);
            break;
        }
        }
    }

    pump_pending_approvals();
    return true;
}


void EventRouter::pump_pending_approvals() {
    // Resolve any approvals queued while a modal was open. Only one per pump
    // so a nested approval (during the approval dialog) queues and resolves
    // on a later tick rather than re-entering this loop.
    if (tui_.modal_open_ || pending_approvals_.empty()) return;
    AgentEvent ev = std::move(pending_approvals_.front());
    pending_approvals_.pop();
    resolve_approval(ev);
}


void EventRouter::resolve_approval(const AgentEvent& ev) {
    agent::Approval d = approve_dialog(ev.text, tui_.policy_timeout_, 0);
    const char* verdict = "denied";
    if (d == agent::Approval::AllowOnce) verdict = "allowed once";
    else if (d == agent::Approval::AllowSession) verdict = "allowed session";
    else if (d == agent::Approval::AlwaysAllow) verdict = "always allow";
    else if (d == agent::Approval::AlwaysDeny) verdict = "always deny";
    tui_.append_line(P_STATUS,
                std::string("approval: ") + verdict + "  (" + ev.text + ")");
    if (ev.approval_promise)
        ev.approval_promise->set_value(d);
}


void EventRouter::on_reasoning(Window* w, const AgentEvent& ev) {
    if (!w) return;
    w->reason_buf += ev.text;
    if (!w->reason_folded && tui_.render_engine_->show_reasoning())
        w->scroll_top = tui_.render_engine_->max_scroll(*w);
}


void EventRouter::on_token(Window* w, const AgentEvent& ev) {
    if (!w) return;
    tui_.render_engine_->clear_working();  // output is displaying — row retires
    if (!w->reason_folded && !w->reason_buf.empty())
        tui_.fold_reasoning(*w);
    w->stream_color = P_ASSISTANT;
    w->stream_buf += ev.text;
    tui_.live_ctx_offset_ += (static_cast<long>(ev.text.size()) / 4) + 1;
    w->scroll_top = tui_.render_engine_->max_scroll(*w);
}


void EventRouter::on_tool_call(Window* w, const AgentEvent& ev) {
    if (!w) return;
    tui_.running_tool_ = ev.tool_name;
    tui_.running_tool_desc_ = tool_display::describe_tool_call(
        ev.tool_name, ev.tool_args);
    tui_.render_engine_->mark_working();
    tui_.flush_stream(*w);
    // One "open" line per advertised call, animated together: round
    // spinner + a human description of what is actually running
    // (bash -> the command, read/write -> path, search -> pattern).
    PendingToolLine pt;
    pt.name = ev.tool_name;
    pt.fingerprint = ev.tool_args.dump();
    pt.frame = 0;
    pt.tail = " " + tool_display::describe_tool_call(ev.tool_name,
                                                     ev.tool_args);
    const char* frame = text::glyph::spinner_round(0);
    pt.window_id = ev.window_id;
    pt.index = tui_.append_line_to(*w, P_STATUS,
                              std::string(frame) + pt.tail);
    pending_tools_.push_back(std::move(pt));
}


void EventRouter::on_tool_result(Window* w, const AgentEvent& ev) {
    if (!w) return;
    tui_.running_tool_.clear();
    tui_.running_tool_desc_.clear();
    // Summary line: colored success/failure icon + one-line report,
    // closed IN PLACE on the open line (single line per tool call).
    rich::Line summary = tool_display::result_line(
        ev.tool_name, ev.tool_args, ev.tool_result.ok,
        ev.tool_result.output, ev.tool_result.error);
    // Match the pending line for this call (same window + name + args, FIFO).
    size_t match = find_pending_tool(ev.window_id, ev.tool_name,
                                     ev.tool_args.dump());
    if (match != std::string::npos) {
        auto& pt = pending_tools_[match];
        Window* ow = tui_.window_by_id(pt.window_id);
        if (ow && pt.index < ow->lines.size()) {
            size_t li = pt.index;
            // Close in place: keep the open line's timestamp run.
            ow->lines[li] = tool_display::close_tool_line(
                ow->lines[li], std::move(summary));
            pending_tools_.erase(pending_tools_.begin() + match);
        } else {
            tui_.append_rich_to(*w, summary);
        }
    } else {
        tui_.append_rich_to(*w, summary);
    }
    // Tool may have modified files — refresh git state for prompt.
    tui_.render_engine_->git_refresh();
}


void EventRouter::on_assistant(Window* w, const AgentEvent& ev) {
    if (!w) return;
    if (w->stream_buf.empty())
        tui_.append_markdown(*w, ev.text);
}


void EventRouter::on_error(Window* w, const AgentEvent& ev) {
    if (!w) return;
    tui_.state_ = agent::RunState::Error;
    tui_.flush_stream(*w);
    tui_.append_line_to(*w, P_STATUS, std::string("error: ") + ev.error_msg);
}


void EventRouter::on_done(Window* w, const AgentEvent& ev) {
    if (!w) return;
    if (tui_.state_ != agent::RunState::Error)
        tui_.state_ = agent::RunState::Idle;
    tui_.running_tool_.clear();
    tui_.flush_stream(*w);
    w->dirty = true;
    tui_.autosave(*w);
}


void EventRouter::on_compress_result(Window* w, const AgentEvent& ev) {
    if (!w) return;
    auto& r = ev.compress_result;
    tui_.state_ = agent::RunState::Idle;
    tui_.ctx_used_.store(static_cast<long>(r.tokens_after));
    {
        std::string s;
        if (r.messages_before == 0) s = "compress: no compressor configured";
        else if (r.messages_after >= r.messages_before)
            s = "compress: nothing to prune (" + std::to_string(r.messages_before) +
                " messages, ~" + std::to_string(r.tokens_before) + " tokens)";
        else
            s = "compress: " + std::to_string(r.messages_before) + " \u2192 " +
                std::to_string(r.messages_after) + " msgs, ~" +
                std::to_string(r.tokens_before) + " \u2192 ~" +
                std::to_string(r.tokens_after) + " tokens  (core:" +
                std::to_string(r.core_count) + " ctx:" +
                std::to_string(r.context_count) + " prune:" +
                std::to_string(r.prune_count) + ")";
        tui_.append_line_to(*w, P_STATUS, s);
    }
    w->dirty = true;
}


size_t EventRouter::find_pending_tool(size_t window_id, const std::string& name,
                              const std::string& fingerprint) const {
    size_t fallback = std::string::npos;
    for (size_t i = 0; i < pending_tools_.size(); ++i) {
        if (pending_tools_[i].window_id != window_id ||
            pending_tools_[i].name != name)
            continue;
        if (pending_tools_[i].fingerprint == fingerprint) return i;
        if (fallback == std::string::npos) fallback = i;
    }
    return fallback;
}

void EventRouter::advance_tool_spinners() {
    if (pending_tools_.empty()) return;
    bool changed = false;
    for (auto& pt : pending_tools_) {
        Window* w = tui_.window_by_id(pt.window_id);
        if (!w || pt.index >= w->lines.size()) {
            pt.index = std::string::npos;
            continue;
        }
        ++pt.frame;
        auto& runs = w->lines[pt.index].runs;
        if (runs.empty()) continue;
        runs.back().text = std::string(text::glyph::spinner_round(pt.frame)) +
                           pt.tail;
        changed = true;
    }
    pending_tools_.erase(
        std::remove_if(pending_tools_.begin(), pending_tools_.end(),
                       [](const PendingToolLine& pt) {
                           return pt.index == std::string::npos;
                       }),
        pending_tools_.end());
    if (changed) tui_.render_engine_->draw();
}

} // namespace tui
