#!/usr/bin/env python3
"""Minimal MCP stdio test server: a real (tiny) MCP server.

Usage:
  mcp_echo.py              — serve tools/resources/prompts
  mcp_echo.py stderr       — write one line to stderr before serving
  mcp_echo.py boom         — write a fatal line to stderr and exit 1

Capabilities: one tool (echo_tool), one resource (doc://greet), one prompt
(greet with required arg `name`). tools/call echoes the arguments back.
"""
import json
import sys

INIT_RESULT = {
    "protocolVersion": "2025-06-18",
    "capabilities": {
        "tools": {"listChanged": True},
        "resources": {},
        "prompts": {},
    },
    "serverInfo": {"name": "echo", "version": "1.0"},
}

TOOLS = [{
    "name": "echo_tool",
    "description": "Echo the arguments back",
    "inputSchema": {
        "type": "object",
        "properties": {
            "text": {"type": "string", "description": "text to echo"},
        },
        "required": ["text"],
    },
}]

RESOURCES = [{
    "uri": "doc://greet",
    "name": "Greeting",
    "description": "A greeting document",
    "mimeType": "text/plain",
}]

PROMPTS = [{
    "name": "greet",
    "title": "Greet",
    "description": "Generate a greeting",
    "arguments": [{"name": "name", "description": "who to greet",
                   "required": True}],
}]


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode == "stderr":
        sys.stderr.write("hello stderr\n")
        sys.stderr.flush()
    if mode == "boom":
        sys.stderr.write("fatal startup error\n")
        sys.stderr.flush()
        sys.exit(1)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        reqid = obj.get("id")
        method = obj.get("method", "")
        if reqid is None:
            continue  # notification
        if method == "initialize":
            result = INIT_RESULT
        elif method == "tools/list":
            result = {"tools": TOOLS}
        elif method == "resources/list":
            result = {"resources": RESOURCES}
        elif method == "prompts/list":
            result = {"prompts": PROMPTS}
        elif method == "tools/call":
            args = obj.get("params", {}).get("arguments", {})
            result = {"content": [
                {"type": "text",
                 "text": "echo:" + str(args.get("text", ""))}
            ]}
        elif method == "resources/read":
            uri = obj.get("params", {}).get("uri", "")
            result = {"contents": [
                {"uri": uri, "mimeType": "text/plain",
                 "text": "hello " + uri}
            ]}
        elif method == "prompts/get":
            args = obj.get("params", {}).get("arguments", {})
            name = str(args.get("name", "world"))
            result = {"messages": [
                {"role": "user",
                 "content": {"type": "text",
                             "text": "greet " + name}}
            ]}
        else:
            result = {"echo": {"method": method,
                               "params": obj.get("params", {})}}
        sys.stdout.write(json.dumps(
            {"jsonrpc": "2.0", "id": reqid, "result": result}) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
