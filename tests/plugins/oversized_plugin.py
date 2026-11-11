#!/usr/bin/env python3
"""A plugin that emits an unterminated line larger than the 64 KiB cap.

AMBER_PLUGIN_TEST_OVERSIZE=initialize makes the handshake oversized; otherwise
the first tool call is.
"""
import json
import os
import sys
import time

MODE = os.environ.get("AMBER_PLUGIN_TEST_OVERSIZE", "")
BLOB = "x" * 70000

for line in sys.stdin:
    msg = json.loads(line)
    method = msg.get("method")
    if method == "initialize":
        if MODE == "initialize":
            sys.stdout.write(BLOB)
            sys.stdout.flush()
            time.sleep(3600)
        sys.stdout.write(
            json.dumps({"id": msg["id"], "result": {"protocol_version": 1, "ok": True}}) + "\n"
        )
        sys.stdout.flush()
    elif method == "tool.call":
        sys.stdout.write(BLOB)
        sys.stdout.flush()
        time.sleep(3600)
    elif method == "shutdown":
        sys.exit(0)
