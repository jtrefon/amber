#include <agent.h>

#include "widgets.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

namespace {

using tui::Dialog;
using tui::FieldSpec;
using tui::Pair;
using tui::form_edit;
using tui::info_dialog;
using tui::menu_select;
using tui::P_ASSISTANT;
using tui::P_BANNER;
using tui::P_STATUS;
using tui::P_REASONING;
using tui::P_USER;

class Tui {
public:
    Tui(agent::Config cfg, agent::ToolRegistry& reg)
        : cfg_(std::move(cfg)), reg_(reg) {
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        set_escdelay(25);
        curs_set(0);
        start_color();
        use_default_colors();
        tui::init_pairs();
    }

    ~Tui() { endwin(); }

    void run() {
        draw();
        banner("cpp-agent - F1 help  F2 config  F3 thinking  F10 settings  "
               "Enter send  PgUp/PgDn scroll  Ctrl-C quit");
        draw_input("");

        // Wake once a second even without input so the status-bar clock ticks.
        // This is a blocking poll with a 1s timeout, not a busy loop: idle CPU
        // is one wakeup per second (negligible).
        timeout(1000);

        std::string input;
        while (true) {
            int ch = getch();
            if (ch == ERR) { tick_clock(); draw_input(input); continue; }
            if (ch == KEY_F(1)) { help_screen(); redraw(input); continue; }
            if (ch == KEY_F(2)) { config_screen(); redraw(input); continue; }
            if (ch == KEY_F(10)) { settings_screen(); redraw(input); continue; }
            if (ch == KEY_F(3)) {
                cfg_.show_reasoning = !cfg_.show_reasoning;
                append_line(P_STATUS, std::string("thinking display: ") +
                                          (cfg_.show_reasoning ? "on" : "off"));
                draw(); draw_input(input); continue;
            }
            if (ch == KEY_NPAGE) {
                scroll_top_ = std::min(max_scroll(),
                                       scroll_top_ + lines_per_page());
                draw(); draw_input(input); continue;
            }
            if (ch == KEY_PPAGE) {
                scroll_top_ = std::max(0, scroll_top_ - lines_per_page());
                draw(); draw_input(input); continue;
            }
            if (ch == 7 || ch == 10 || ch == 13 || ch == KEY_ENTER) {
                if (input.empty()) continue;
                std::string prompt = input;
                append_line(P_USER, "> " + input);
                input.clear();
                draw_input("");
                send(prompt);
                draw_input("");
                continue;
            }
            if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (!input.empty()) input.pop_back();
                draw_input(input);
            } else if (ch >= 32 && ch <= 126) {
                input += static_cast<char>(ch);
                draw_input(input);
            }
        }
    }

private:
    int height() const { int y, x; getmaxyx(stdscr, y, x); (void)x; return y; }
    int width() const { int y, x; getmaxyx(stdscr, y, x); (void)y; return x; }
    int lines_per_page() const { return std::max(1, height() - 2); }
    int stream_lines() const {
        return stream_buf_.empty()
                   ? 0
                   : static_cast<int>(wrap_text(stream_buf_, width()).size());
    }
    int max_scroll() const {
        int m = static_cast<int>(lines_.size()) + stream_lines() - (height() - 2);
        return m < 0 ? 0 : m;
    }

    void redraw(const std::string& input) {
        touchwin(stdscr);
        draw();
        draw_input(input);
    }

    // Wrap `text` into display lines: honour embedded newlines, then word-wrap
    // each paragraph to `w` columns (falling back to hard splits for words
    // longer than the width). Tabs are expanded to spaces.
    // Byte length of the UTF-8 sequence starting at s[i] (1 on invalid lead).
    static size_t utf8_len(const std::string& s, size_t i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2
                 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        // Validate continuation bytes; treat a truncated sequence as 1 byte.
        for (size_t k = 1; k < n; ++k)
            if (i + k >= s.size() ||
                (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                return 1;
        return n;
    }

    static std::vector<std::string> wrap_text(const std::string& text, int w) {
        if (w <= 0) w = 80;
        std::vector<std::string> out;
        // Sanitize: expand tabs, drop CR, strip ANSI/control bytes that would
        // otherwise be written raw to the terminal (garbage on screen), while
        // preserving valid multibyte UTF-8 (emoji/CJK) intact.
        std::string src;
        src.reserve(text.size());
        for (size_t i = 0; i < text.size();) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c == '\t') { src += "    "; ++i; continue; }
            if (c == '\n') { src += '\n'; ++i; continue; }
            if (c == 0x1b) {                       // ESC: skip a CSI/simple seq
                ++i;
                if (i < text.size() && text[i] == '[') {
                    ++i;
                    while (i < text.size() &&
                           !(text[i] >= '@' && text[i] <= '~')) ++i;
                    if (i < text.size()) ++i;      // final byte
                } else if (i < text.size()) {
                    ++i;
                }
                continue;
            }
            if (c < 0x20 || c == 0x7f) { ++i; continue; }  // other control chars
            size_t n = utf8_len(text, i);
            if (n == 1 && c >= 0x80) { src += '?'; ++i; continue; } // bad byte
            src.append(text, i, n);
            i += n;
        }
        size_t start = 0;
        while (start <= src.size()) {
            size_t nl = src.find('\n', start);
            std::string para = (nl == std::string::npos)
                                   ? src.substr(start)
                                   : src.substr(start, nl - start);
            // word-wrap this paragraph
            if (para.empty()) {
                out.push_back("");
            } else {
                size_t p = 0;
                while (p < para.size()) {
                    // Walk forward up to `w` display columns, counting whole
                    // UTF-8 characters (each counted as one column) so we never
                    // slice through a multibyte sequence.
                    size_t q = p;
                    int cols = 0;
                    while (q < para.size() && cols < w) {
                        q += utf8_len(para, q);
                        ++cols;
                    }
                    if (q >= para.size()) {
                        out.push_back(para.substr(p));
                        break;
                    }
                    // find a space to break on within [p, q]
                    size_t brk = para.rfind(' ', q);
                    if (brk == std::string::npos || brk <= p) {
                        out.push_back(para.substr(p, q - p));  // hard split
                        p = q;
                    } else {
                        out.push_back(para.substr(p, brk - p));
                        p = brk + 1;                           // skip the space
                    }
                }
            }
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        return out;
    }

    // IRC (BitchX/ircII) style local timestamp, e.g. "[23:04] ".
    static std::string timestamp() {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        char buf[16];
        std::strftime(buf, sizeof(buf), "[%H:%M] ", &tm);
        return buf;
    }

    // Commit a chat message prefixed with an IRC-style timestamp. The stamp is
    // shown once on the first line; wrapped continuation lines are indented to
    // align under the text, matching ircII/BitchX behaviour.
    void append_line(int color, const std::string& text) {
        append_line_ts(color, text, timestamp());
    }

    // As append_line, but with a caller-supplied timestamp so a streamed reply
    // commits under the same stamp it was shown with while streaming.
    void append_line_ts(int color, const std::string& text,
                        const std::string& ts) {
        const std::string pad(ts.size(), ' ');
        int avail = std::max(1, width() - static_cast<int>(ts.size()));
        auto wrapped = wrap_text(text, avail);
        if (wrapped.empty()) wrapped.push_back("");
        for (size_t i = 0; i < wrapped.size(); ++i)
            lines_.push_back({color, (i == 0 ? ts : pad) + wrapped[i]});
        if (lines_.size() > 10000)
            lines_.erase(lines_.begin(), lines_.begin() + 5000);
        scroll_top_ = max_scroll();
    }

    void banner(const std::string& text) {
        lines_.push_back({P_BANNER, text});
        scroll_top_ = max_scroll();
    }

    void draw() {
        erase();
        int view_h = height() - 2;

        // Build the full display list = committed lines + live (uncommitted)
        // streaming buffer wrapped to the current width. The stream buffer is
        // rendered in place and only committed once complete.
        std::vector<std::pair<int, std::string>> pending;
        const std::string ts = stream_ts_.empty() ? timestamp() : stream_ts_;
        const std::string pad(ts.size(), ' ');
        int avail = std::max(1, width() - static_cast<int>(ts.size()));
        auto push_wrapped = [&](int color, const std::string& body) {
            auto ls = wrap_text(body, avail);
            for (size_t i = 0; i < ls.size(); ++i)
                pending.push_back({color, (i == 0 ? ts : pad) + ls[i]});
        };
        if (show_reasoning_ && !reason_folded_ && !reason_buf_.empty()) {
            pending.push_back({P_REASONING, ts + "thinking..."});
            for (auto& l : wrap_text(reason_buf_, avail))
                pending.push_back({P_REASONING, pad + l});
        }
        if (!stream_buf_.empty())
            push_wrapped(stream_color_, stream_buf_);

        int total = static_cast<int>(lines_.size() + pending.size());
        int max_top = std::max(0, total - view_h);
        int start = std::min(scroll_top_, max_top);

        for (int row = 0; row < view_h; ++row) {
            int idx = start + row;
            if (idx < 0 || idx >= total) continue;
            const auto& [color, text] =
                (idx < static_cast<int>(lines_.size()))
                    ? lines_[idx]
                    : pending[idx - lines_.size()];
            bool dim = (color == P_REASONING);
            attron(COLOR_PAIR(color) | (dim ? A_DIM : 0));
            mvaddnstr(row, 0, text.c_str(), width());
            attroff(COLOR_PAIR(color) | (dim ? A_DIM : 0));
        }

        std::string st = " lines:" + std::to_string(total) +
                         " top:" + std::to_string(start + 1);
        draw_status_bar(st);
        refresh();
    }

    // Render the blue status bar with left-aligned info and an IRC-style clock
    // pinned to the right. Cheap enough to call once a second for a live tick.
    void draw_status_bar(const std::string& left) {
        int w = width();
        int y = height() - 2;
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        char clk[16];
        std::strftime(clk, sizeof(clk), "[%H:%M:%S]", &tm);
        std::string clock = clk;

        attron(COLOR_PAIR(P_BANNER));
        mvhline(y, 0, ' ', w);
        mvaddnstr(y, 0, left.c_str(),
                  std::max(0, w - static_cast<int>(clock.size()) - 1));
        if (static_cast<int>(clock.size()) < w)
            mvaddnstr(y, w - static_cast<int>(clock.size()), clock.c_str(),
                      static_cast<int>(clock.size()));
        attroff(COLOR_PAIR(P_BANNER));
    }

    // Update only the status bar (for the once-per-second clock tick) without
    // rebuilding the whole scrollback view.
    void tick_clock() {
        int total = static_cast<int>(lines_.size());
        if (!stream_buf_.empty()) total += stream_lines();
        int start = std::min(scroll_top_,
                             std::max(0, total - (height() - 2)));
        draw_status_bar(" lines:" + std::to_string(total) +
                        " top:" + std::to_string(start + 1));
        refresh();
    }

    void draw_input(const std::string& s) {
        int y = height() - 1;
        move(y, 0);
        clrtoeol();
        attron(COLOR_PAIR(P_USER));
        std::string shown = "prompt> " + s;
        mvaddnstr(y, 0, shown.c_str(), width());
        attroff(COLOR_PAIR(P_USER));
        refresh();
    }

    void send(const std::string& prompt) {
        agent::AgentHooks hooks;
        reason_buf_.clear();
        reason_folded_ = false;
        show_reasoning_ = cfg_.show_reasoning;
        stream_ts_ = timestamp();   // stamp the reply the moment it starts
        hooks.on_reasoning = [this](const std::string& d) {
            // Live thinking: accumulate and render dim, above the answer.
            reason_buf_ += d;
            scroll_top_ = max_scroll();
            draw();
        };
        hooks.on_token = [this](const std::string& d) {
            // First answer token: fold any thinking into one collapsible summary
            // line so it stops occupying the viewport.
            if (!reason_folded_ && !reason_buf_.empty()) {
                fold_reasoning();
            }
            // Accumulate the partial message and re-render it live in place.
            // Do NOT commit per token (that made each fragment its own line).
            stream_color_ = P_ASSISTANT;
            stream_buf_ += d;
            scroll_top_ = max_scroll();   // auto-follow
            draw();
        };
        hooks.on_assistant = [this](const std::string& s) {
            // Non-streaming path: nothing was streamed, so commit the whole msg.
            if (stream_buf_.empty()) append_line(P_ASSISTANT, s);
        };
        hooks.on_status = [this](const std::string& s) { append_line(P_STATUS, s); };
        hooks.on_tool_call = [this](const std::string& n, const agent::json& a) {
            flush_stream();
            append_line(P_STATUS, "tool: " + n + " " + a.dump());
        };
        hooks.on_tool_result = [this](const std::string& n, const agent::ToolResult& r) {
            append_line(P_STATUS, "result:" + n + " " + (r.ok ? r.output : r.error));
        };
        try {
            agent::Agent agent(cfg_, reg_, hooks);
            agent.run(prompt);
        } catch (const std::exception& e) {
            flush_stream();
            append_line(P_STATUS, std::string("error: ") + e.what());
        }
        flush_stream();
        draw();
    }

    // Collapse the live thinking buffer into a single dim summary line. The
    // full reasoning is preserved in the telemetry log, not the viewport.
    void fold_reasoning() {
        if (reason_folded_) return;
        reason_folded_ = true;
        if (reason_buf_.empty()) return;
        size_t words = 1;
        for (char ch : reason_buf_) if (ch == ' ') ++words;
        append_line_ts(P_REASONING,
                       "[thought for " + std::to_string(words) + " words]",
                       stream_ts_.empty() ? timestamp() : stream_ts_);
        reason_buf_.clear();
    }

    void flush_stream() {
        // If we finished on pure thinking (no answer streamed), still fold it.
        if (!reason_folded_ && !reason_buf_.empty()) fold_reasoning();
        if (stream_buf_.empty()) return;
        append_line_ts(stream_color_, stream_buf_,
                       stream_ts_.empty() ? timestamp() : stream_ts_);
        stream_buf_.clear();
        stream_ts_.clear();
        draw();
    }

    void help_screen() {
        info_dialog("Help", {
            "F1        show this help",
            "F2        show configuration",
            "F3        toggle live thinking display",
            "F10       server settings (URL, token, model)",
            "Enter     send the prompt",
            "Ctrl-G    send the prompt",
            "PgUp/PgDn scroll scrollback",
            "Ctrl-C    quit",
            "",
            "Tools: read (paginated), write (patch), search (grep/semantic).",
        });
    }

    void config_screen() {
        auto mask = [](const std::string& s) {
            return s.empty() ? std::string("(unset)") : std::string(s.size(), '*');
        };
        info_dialog("Configuration", {
            "api_base:  " + cfg_.api_base,
            "api_key:   " + mask(cfg_.api_key),
            "model:     " + cfg_.model,
            "stream:    " + std::string(cfg_.stream ? "on" : "off"),
            "max_iter:  " + std::to_string(cfg_.max_tool_iterations),
            "system:    " + cfg_.system_prompt_path,
            "tools:     " + cfg_.tools_prompt_path,
        });
    }

    // Server settings using a native libform dialog.
    void settings_screen() {
        std::vector<FieldSpec> fields = {
            {"Server URL", cfg_.api_base, false},
            {"Token", cfg_.api_key, true},
            {"Model", cfg_.model, false},
        };
        if (!form_edit("Server settings", fields)) return;

        cfg_.api_base = fields[0].value;
        cfg_.api_key = fields[1].value;
        cfg_.model = fields[2].value;

        std::vector<std::string> post = {"Save to " + settings_path_,
                                         "Apply only (don't save)"};
        int choice = menu_select("Apply settings?", post);
        if (choice == 0) {
            save_settings();
            append_line(P_STATUS, "settings saved to " + settings_path_);
        } else if (choice == 1) {
            append_line(P_STATUS, "settings applied (not saved)");
        }
    }

    void save_settings() {
        std::ofstream f(settings_path_, std::ios::trunc);
        if (!f) return;
        f << "# cpp-agent settings\n";
        f << "api_base=" << cfg_.api_base << "\n";
        f << "api_key=" << cfg_.api_key << "\n";
        f << "model=" << cfg_.model << "\n";
        f << "system_prompt=" << cfg_.system_prompt_path << "\n";
        f << "tools_prompt=" << cfg_.tools_prompt_path << "\n";
    }

    agent::Config cfg_;
    agent::ToolRegistry& reg_;
    std::string settings_path_ = "cpp-agent.conf";
    std::vector<std::pair<int, std::string>> lines_;
    int scroll_top_ = 0;
    std::string stream_buf_;
    int stream_color_ = P_ASSISTANT;
    std::string stream_ts_;         // timestamp captured when a reply begins
    std::string reason_buf_;        // live thinking text, before folding
    bool reason_folded_ = false;    // collapsed to a summary line yet?
    bool show_reasoning_ = true;    // toggle live thinking display
};

} // namespace

int main(int argc, char** argv) {
    agent::Config cfg;
    std::string config_file;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config_file = argv[++i];
        else if (a == "--api-base" && i + 1 < argc) cfg.api_base = argv[++i];
        else if (a == "--api-key" && i + 1 < argc) cfg.api_key = argv[++i];
        else if (a == "--model" && i + 1 < argc) cfg.model = argv[++i];
        else if (a == "--system" && i + 1 < argc) cfg.system_prompt_path = argv[++i];
        else if (a == "--tools" && i + 1 < argc) cfg.tools_prompt_path = argv[++i];
        else if (a == "--no-stream") cfg.stream = false;
    }
    if (!config_file.empty()) cfg.load(config_file);
    {
        std::ifstream sf("cpp-agent.conf");
        if (sf) cfg.load("cpp-agent.conf");
    }
    cfg.apply_environment();

    if (cfg.system_prompt_path.empty()) cfg.system_prompt_path = "prompts/system.md";
    if (cfg.tools_prompt_path.empty()) cfg.tools_prompt_path = "prompts/tools.md";

    agent::ToolRegistry registry;
    agent::register_default_tools(registry);

    Tui tui(cfg, registry);
    tui.run();
    return 0;
}
