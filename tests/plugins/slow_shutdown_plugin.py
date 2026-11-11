#!/usr/bin/env python3
"""A plugin that ignores the shutdown notification.

Teardown gets a one-second grace window and then SIGKILL; the host must not
wait for this sleep.
"""
import json
import sys
import time

for line in sys.stdin:
    msg = json.loads(line)
    method = msg.get("method")
    if method == "initialize":
        sys.stdout.write(
            json.dumps({"id": msg["id"], "result": {"protocol_version": 1, "ok": True}}) + "\n"
        )
        sys.stdout.flush()
    elif method == "tool.call":
        sys.stdout.write(json.dumps({"id": msg["id"], "result": {"ok": True, "output": "ok"}}) + "\n")
        sys.stdout.flush()
    elif method == "shutdown":
        time.sleep(30)
        sys.exit(0)
