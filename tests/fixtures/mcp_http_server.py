#!/usr/bin/env python3
"""Minimal MCP Streamable HTTP test server.

Usage: mcp_http_server.py <statefile> <mode>
Writes "PORT:<port>\nPID:<pid>\n" to <statefile> once listening.

Modes:
  echo     — answer every request with application/json {echo: {...}}
  sse      — answer requests with text/event-stream: a progress notification,
             then the response, then close
  session  — assign Mcp-Session-Id "sess-1" on initialize; require it on
             subsequent requests (400 otherwise); expire after 3 requests (404)
"""
import http.server
import json
import os
import socketserver
import sys
import threading

SESSION_ID = "sess-1"


def make_handler(statefile, mode):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        counter = 0

        def log_message(self, fmt, *args):
            pass

        def _read_body(self):
            length = int(self.headers.get("Content-Length", "0"))
            return self.rfile.read(length) if length else b""

        def _json_reply(self, obj, status=200, extra_headers=None):
            body = json.dumps(obj).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            for k, v in (extra_headers or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def _accepted(self):
            self.send_response(202)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def _require_session(self, msg):
            if self.headers.get("Mcp-Session-Id") != SESSION_ID:
                self._json_reply(
                    {"jsonrpc": "2.0", "error": {"code": -32600,
                                                 "message": "bad session"}},
                    status=400)
                return False
            return True

        def do_POST(self):
            Handler.counter += 1
            body = self._read_body()
            try:
                obj = json.loads(body) if body else {}
            except ValueError:
                self._json_reply(
                    {"jsonrpc": "2.0",
                     "error": {"code": -32700, "message": "parse error"}},
                    status=400)
                return
            method = obj.get("method", "")
            reqid = obj.get("id")

            if mode == "session" and method != "initialize":
                if not self._require_session(obj):
                    return
                if Handler.counter >= 4:
                    self._json_reply(
                        {"jsonrpc": "2.0",
                         "error": {"code": -32000, "message": "gone"}},
                        status=404)
                    return

            if reqid is None:
                self._accepted()
                return

            if method == "initialize":
                result = {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {"tools": {"listChanged": True}},
                    "serverInfo": {"name": "fixture", "version": "1.0"},
                }
                if mode == "session":
                    result["sessionSeen"] = self.headers.get(
                        "Mcp-Session-Id", "")
                self._json_reply({"jsonrpc": "2.0", "id": reqid,
                                  "result": result},
                                 extra_headers={"Mcp-Session-Id": SESSION_ID}
                                 if mode == "session" else None)
                return

            if mode == "sse":
                payload = (
                    "event: message\n"
                    "data: {\"jsonrpc\":\"2.0\","
                    "\"method\":\"notifications/progress\","
                    "\"params\":{\"progressToken\":\"p1\",\"progress\":1}}\n"
                    "\n"
                    "event: message\n"
                    "data: {\"jsonrpc\":\"2.0\",\"id\":" +
                    json.dumps(reqid) +
                    ",\"result\":{\"sse\":true,\"session\":" +
                    json.dumps(self.headers.get("Mcp-Session-Id", "")) +
                    "}}\n"
                    "\n")
                body_bytes = payload.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(body_bytes)))
                self.end_headers()
                self.wfile.write(body_bytes)
                return

            result = {"echo": {"method": method, "params": obj.get("params", {})}}
            if mode == "session":
                result["session"] = self.headers.get("Mcp-Session-Id", "")
            self._json_reply({"jsonrpc": "2.0", "id": reqid, "result": result})

        def do_DELETE(self):
            self._accepted()

    return Handler


def main():
    statefile = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else "echo"
    handler = make_handler(statefile, mode)

    class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True

    server = Server(("127.0.0.1", 0), handler)
    port = server.server_address[1]
    with open(statefile, "w") as f:
        f.write("PORT:%d\nPID:%d\n" % (port, os.getpid()))
    server.serve_forever()


if __name__ == "__main__":
    main()
