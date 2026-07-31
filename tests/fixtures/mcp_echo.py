#!/usr/bin/env python3
"""Minimal MCP stdio test server: echoes every request as a response.

Usage:
  mcp_echo.py              — echo loop
  mcp_echo.py stderr       — write one line to stderr before the loop
  mcp_echo.py boom         — write a fatal line to stderr and exit 1
"""
import json
import sys


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
        if "id" in obj and "method" in obj:
            reply = {
                "jsonrpc": "2.0",
                "id": obj["id"],
                "result": {
                    "echo": {
                        "method": obj["method"],
                        "params": obj.get("params", {}),
                    }
                },
            }
            sys.stdout.write(json.dumps(reply) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
