// cdp plugin — real browser automation via the Chrome DevTools Protocol.
// Standalone executable speaking the amber plugin protocol over stdio; talks
// to a CDP endpoint (default ws://127.0.0.1:9222) over its own WebSocket
// client. The core app never learns about CDP or WebSockets.
#include "ws_client.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

using json = nlohmann::json;

namespace {

constexpr int kCdpTimeoutMs = 10000;

std::string endpoint = "ws://127.0.0.1:9222";
std::string workspace;

cdp::WsClient ws;
int next_cdp_id = 1;

json result(bool ok, const std::string& output, const json& meta = json::object()) {
    return {{"ok", ok}, {"output", output}, {"meta", meta}};
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// HTTP base derived from the ws endpoint (ws://host:port/path → http://host:port).
std::string http_base() {
    std::string base = endpoint;
    if (base.rfind("ws://", 0) == 0) base = "http://" + base.substr(5);
    size_t slash = base.find('/', 7);
    if (slash != std::string::npos) base.resize(slash);
    return base;
}

std::string ws_json_get(const std::string& url) {
    cdp::WsClient tmp;
    std::string body, err;
    if (tmp.http_get_url(url, body, err)) return body;
    return "";
}

json cdp_command(const std::string& method, const json& params) {
    json req = {{"id", next_cdp_id++}, {"method", method}, {"params", params}};
    if (!ws.send_text(req.dump())) return json::object();
    std::string reply;
    while (ws.recv_text(reply, kCdpTimeoutMs)) {
        json r = json::parse(reply, nullptr, false);
        if (r.is_discarded()) continue;
        if (r.contains("id") && r["id"] == req["id"]) return r;
    }
    return json::object();
}

// Lazily attach to the first page target.
bool ensure_target(std::string& err) {
    if (ws.connected()) return true;
    std::string body = ws_json_get(http_base() + "/json");
    json targets = json::parse(body, nullptr, false);
    if (targets.is_discarded() || !targets.is_array()) {
        err = "cannot reach CDP endpoint at " + endpoint +
              " (is Chrome running with --remote-debugging-port?)";
        return false;
    }
    std::string ws_url;
    for (const auto& t : targets) {
        if (t.value("type", std::string()) == "page" &&
            t.contains("webSocketDebuggerUrl"))
            ws_url = t["webSocketDebuggerUrl"].get<std::string>();
        if (!ws_url.empty()) break;
    }
    if (ws_url.empty()) {
        err = "no page target found (open a tab in Chrome first)";
        return false;
    }
    if (!ws.connect(ws_url, err)) return false;
    return true;
}

std::string tool_list_targets() {
    std::string body = ws_json_get(http_base() + "/json");
    json targets = json::parse(body, nullptr, false);
    if (targets.is_discarded() || !targets.is_array())
        return "ERROR: cannot reach CDP endpoint at " + endpoint;
    std::string out;
    for (const auto& t : targets)
        out += t.value("type", "?") + " | " + t.value("title", "") + " | " +
               t.value("url", "") + "\n";
    return trim(out);
}

std::string tool_navigate(const json& args) {
    std::string err;
    if (!ensure_target(err)) return "ERROR: " + err;
    json r = cdp_command("Page.navigate", {{"url", args.value("url", std::string())}});
    if (r.empty()) return "ERROR: no response from browser";
    if (r.contains("error"))
        return "ERROR: " + r["error"].value("message", std::string());
    std::string id = r.value("result", json()).value("frameId", std::string());
    return "navigating to " + args.value("url", std::string()) + (id.empty() ? "" : " (frame " + id + ")");
}

std::string tool_eval(const json& args) {
    std::string err;
    if (!ensure_target(err)) return "ERROR: " + err;
    json r = cdp_command("Runtime.evaluate",
                         {{"expression", args.value("expression", std::string())},
                          {"returnByValue", true}});
    if (r.empty()) return "ERROR: no response from browser";
    if (r.contains("error"))
        return "ERROR: " + r["error"].value("message", std::string());
    const json& res = r["result"];
    if (res.contains("exceptionDetails"))
        return "ERROR: " + res["exceptionDetails"]
                              .value("text", std::string());
    if (res.contains("result")) {
        const json& v = res["result"];
        if (v.contains("value"))
            return v["value"].is_string() ? v["value"].get<std::string>()
                                          : v["value"].dump();
        return "undefined";
    }
    return "";
}

std::string tool_url() {
    std::string err;
    if (!ensure_target(err)) return "ERROR: " + err;
    json r = cdp_command("Runtime.evaluate",
                         {{"expression", "location.href"}, {"returnByValue", true}});
    if (r.empty() || !r.contains("result")) return "ERROR: no response";
    return r["result"].value("result", json()).value("value", "unknown");
}

std::string tool_click(const json& args) {
    std::string err;
    if (!ensure_target(err)) return "ERROR: " + err;
    int x = args.value("x", 0);
    int y = args.value("y", 0);
    cdp_command("Input.dispatchMouseEvent",
                {{"type", "mousePressed"}, {"x", x}, {"y", y}, {"button", "left"},
                 {"clickCount", 1}});
    cdp_command("Input.dispatchMouseEvent",
                {{"type", "mouseReleased"}, {"x", x}, {"y", y}, {"button", "left"},
                 {"clickCount", 1}});
    return "clicked at (" + std::to_string(x) + ", " + std::to_string(y) + ")";
}

std::string tool_type(const json& args) {
    std::string err;
    if (!ensure_target(err)) return "ERROR: " + err;
    cdp_command("Input.insertText", {{"text", args.value("text", std::string())}});
    return "typed " + std::to_string(args.value("text", std::string()).size()) + " chars";
}

std::string base64_decode(const std::string& in) {
    static const std::array<int, 256> tbl = [] {
        std::array<int, 256> t{};
        t.fill(-1);
        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; chars[i]; ++i) t[(unsigned char)chars[i]] = i;
        return t;
    }();
    std::string out;
    int val = 0, bits = 0;
    for (unsigned char c : in) {
        if (tbl[c] < 0) continue;
        val = (val << 6) | tbl[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((val >> bits) & 0xff);
        }
    }
    return out;
}

std::string tool_screenshot(const json& args) {
    std::string err;
    if (!ensure_target(err)) return "ERROR: " + err;
    json p = {{"format", "png"}, {"fromSurface", true}};
    if (args.contains("width")) p["width"] = args["width"];
    if (args.contains("height")) p["height"] = args["height"];
    json r = cdp_command("Page.captureScreenshot", p);
    if (r.empty()) return "ERROR: no response from browser";
    if (!r.contains("result"))
        return "ERROR: " + r.value("error", json()).value("message", std::string());
    std::string b64 = r["result"].value("data", std::string());
    std::string png = base64_decode(b64);
    if (png.empty()) return "ERROR: screenshot returned no data";
    std::string dir = workspace.empty() ? "." : workspace + "/.amber";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    long ts = static_cast<long>(std::chrono::system_clock::now()
                                   .time_since_epoch().count() / 1000000000LL);
    std::string path = dir + "/cdp_" + std::to_string(ts) + ".png";
    std::ofstream f(path, std::ios::binary);
    f << png;
    return "saved screenshot: " + path + " (" + std::to_string(png.size()) + " bytes)";
}

std::string tool_snapshot() {
    std::string err;
    if (!ensure_target(err)) return "ERROR: " + err;
    json r = cdp_command("Runtime.evaluate",
                         {{"expression", "document.documentElement.outerHTML"},
                          {"returnByValue", true}});
    if (r.empty()) return "ERROR: no response from browser";
    std::string html = r.value("result", json()).value("result", json())
                           .value("value", std::string());
    if (html.empty()) return "ERROR: empty page snapshot";
    return html.substr(0, 12000) + (html.size() > 12000 ? "\n...[truncated]" : "");
}

json dispatch(const std::string& name, const json& args) {
    if (name == "list_targets") return result(true, tool_list_targets());
    if (name == "navigate") return result(true, tool_navigate(args));
    if (name == "eval") return result(true, tool_eval(args));
    if (name == "url") return result(true, tool_url());
    if (name == "click") return result(true, tool_click(args));
    if (name == "type") return result(true, tool_type(args));
    if (name == "screenshot") return result(true, tool_screenshot(args));
    if (name == "snapshot") return result(true, tool_snapshot());
    return result(false, "ERROR: unknown tool " + name);
}

} // namespace

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        json msg = json::parse(line, nullptr, false);
        if (msg.is_discarded()) continue;
        std::string method = msg.value("method", std::string());
        json resp = json::object();
        resp["id"] = msg.value("id", json());
        if (method == "initialize") {
            endpoint = msg["params"].value("endpoint", endpoint);
            if (!endpoint.empty() && endpoint[0] != 'w') endpoint = "ws://" + endpoint;
            workspace = msg["params"].value("workspace", std::string());
            resp["result"] = {{"protocol_version", 1}, {"ok", true}};
        } else if (method == "tool.call") {
            resp["result"] = dispatch(
                msg["params"].value("name", std::string()),
                msg["params"].contains("args") ? msg["params"]["args"] : json::object());
        } else if (method == "shutdown") {
            ws.close();
            break;
        } else {
            resp["result"] = result(false, "ERROR: unknown method " + method);
        }
        std::cout << resp.dump() << "\n";
        std::cout.flush();
    }
    return 0;
}
